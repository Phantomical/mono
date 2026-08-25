#include "tier-counter.hpp"

#include "../mono_lsda_format.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

#include <cstdint>
#include <iterator>
#include <string>

using namespace llvm;

namespace mono {
namespace {

/*
 * A tier-1 body counts what it spends and asks for tier 2 when the count runs
 * out. One unit is one instruction that emits code, and a call costs the entry
 * weight on top. The body adds up the turns of its loops in a register:
 *
 *         cost -= constant                    in the entry block
 *         acc = 0                             in the entry block
 *         acc += weight (loop)                in each loop header
 *         cost -= acc                         at each exit
 *
 * The constant is the weight of the blocks no loop holds, plus the entry weight.
 * So one counter reaches a body that is hot and a body that is heavy, which a
 * count of one kind alone does not. A count of calls says nothing about how long
 * a method runs, and euler keeps Euler.Tunnel:calculateR () at tier 1 for a whole
 * run that way. A count of work says nothing about how often a method is called,
 * and a body of three instructions never reaches one however hot it is:
 * SharpChess is full of those, property getters called millions of times, and
 * they are the methods whose promotion pays there.
 *
 * The entry charges the constant rather than the exits, because a body has a
 * third way out. A callee's exception can unwind through the frame while no
 * clause here catches it, and no instruction the translator wrote marks that
 * point. The entry is the one point every call runs, so what it charges survives
 * an exit of any kind.
 *
 * The accumulator needs an exit of its own there, and that is the pad below. A
 * body with a loop and a call that can unwind gets a fault clause over the whole
 * of it. Its handler charges the turns, then calls mono_llvm_resume_unwind ().
 * Each call collect_unwinding_calls () takes becomes an invoke on to that pad,
 * which is what makes the clause table cover it.
 *
 * A call of a body no loop has any weight in costs the constant and no more, so
 * such a body carries no accumulator and no write-back. That is most methods.
 * Leaving the write-backs in them is expensive: pystone under IronPython reads
 * +8.9% of CPU with a write-back in every body against -1.1% with them in the
 * bodies that have a loop.
 *
 * Placement is per loop rather than per block, because no pass behind this one
 * tidies up what it writes, and tier-1 codegen is FastISel with the fast register
 * allocator. One add for each loop keeps the live range short enough to stay in a
 * register. A loop charges the weight of its own blocks once for each turn, which
 * is exact for a straight-line body and too much for a branchy one.
 */

/// Counts the instructions in block that emit code.
///
/// A PHI is a register copy at worst. The assume-like intrinsics and the debug
/// records emit nothing at all. InstrProfilingLoweringPass has already turned the
/// profile instrumentation into ordinary arithmetic, which does count: the body
/// really does run it.
uint64_t
block_weight (const BasicBlock &block)
{
	uint64_t weight = 0;

	for (const Instruction &i : block)
		if (!isa<PHINode> (&i) && !i.isDebugOrPseudoInst ()
		    && !isAssumeLikeIntrinsic (&i))
			++weight;

	return weight;
}

/// Finds the instruction a ret's write-back goes in front of.
///
/// A call marked tail or musttail must keep the ret immediately behind it, so the
/// write-back goes in front of such a call. Codegen refuses a musttail call it
/// cannot turn into a jump, and an instruction between a tail call and its ret
/// takes the mark's value away. LLVM lets a bitcast stand between the two.
Instruction *
write_back_point (ReturnInst *ret)
{
	for (Instruction *i = ret->getPrevNode (); i != nullptr; i = i->getPrevNode ()) {
		if (auto *call = dyn_cast<CallInst> (i)) {
			CallInst::TailCallKind kind = call->getTailCallKind ();

			if (kind == CallInst::TCK_Tail || kind == CallInst::TCK_MustTail)
				return call;

			break;
		}

		if (!isa<BitCastInst> (i))
			break;
	}

	return ret;
}

/// Collects the instruction each write-back goes in front of.
///
/// A ret is one. A plain call to a noreturn callee, with unreachable behind it,
/// is the other: that is the shape the translator gives a site the exception
/// leaves the frame from for good. The same site becomes an invoke when a clause
/// in this method protects it (method-to-llvm/exceptions.cpp). An invoke has not
/// left the frame. A write-back in front of one takes the same work off the
/// counter again when the ret is reached.
///
/// A callee's exception that unwinds through this frame reaches none of these
/// points. emit_unwind_pad () is where that exit charges instead.
void
collect_write_backs (Function &f, SmallVectorImpl<Instruction *> &points)
{
	for (BasicBlock &block : f) {
		Instruction *terminator = block.getTerminator ();

		if (auto *ret = dyn_cast<ReturnInst> (terminator)) {
			points.push_back (write_back_point (ret));
			continue;
		}

		if (!isa<UnreachableInst> (terminator))
			continue;

		auto *call = dyn_cast_or_null<CallInst> (terminator->getPrevNode ());

		if (call != nullptr && call->doesNotReturn ())
			points.push_back (call);
	}
}

/// Adds each loop's weight into per_loop, and answers with the weight of the
/// blocks no loop holds.
///
/// The entry block lies in no loop, because it has no predecessor, so the answer
/// is never zero.
uint64_t
weigh (Function &f, LoopInfo &li, DenseMap<const Loop *, uint64_t> &per_loop)
{
	uint64_t acyclic = 0;

	for (BasicBlock &block : f) {
		uint64_t weight = block_weight (block);

		if (weight == 0)
			continue;

		// The innermost loop, which is what makes a loop's total the weight of
		// the blocks it holds and its sub-loops do not.
		if (const Loop *loop = li.getLoopFor (&block))
			per_loop[loop] += weight;
		else
			acyclic += weight;
	}

	return acyclic;
}

/// Puts the accumulator in, and adds each loop's weight at its header.
///
/// The accumulator is a stack slot, which PromoteMemToReg turns into phis once
/// the write-backs are in. Each store is the last value plus a step, so a slot
/// needs none of the bookkeeping an SSAUpdater wants.
/// ProfileCounterPromoterPass builds its own accumulators the same way.
AllocaInst *
emit_accumulator (Function &f, LoopInfo &li,
                  const DenseMap<const Loop *, uint64_t> &per_loop)
{
	Type *i64 = Type::getInt64Ty (f.getContext ());
	BasicBlock &entry = f.getEntryBlock ();
	IRBuilder<> at_entry (&entry, entry.getFirstInsertionPt ());
	AllocaInst *slot = at_entry.CreateAlloca (i64, nullptr, "tier_cost.promoted");

	at_entry.CreateStore (ConstantInt::get (i64, 0), slot);

	for (Loop *loop : li.getLoopsInPreorder ()) {
		uint64_t weight = per_loop.lookup (loop);

		if (weight == 0)
			continue;

		/*
		 * The header, because a natural loop has exactly one and every turn
		 * runs it. So one insert point covers the loop whatever its latches
		 * are, and a loop with several latches still pays one add.
		 */
		BasicBlock *header = loop->getHeader ();
		IRBuilder<> at_header (header, header->getFirstInsertionPt ());
		Value *sum = at_header.CreateAdd (at_header.CreateLoad (i64, slot),
		                                  ConstantInt::get (i64, weight));

		at_header.CreateStore (sum, slot);
	}

	return slot;
}

/*
 * if (counter > 0)
 *         if ((counter -= cost) <= 0)
 *                 promote (method);
 *
 * The plain load in front keeps the atomic off the path a body takes once the
 * counter is spent. That is every exit after the method has asked.
 *
 * atomicrmw answers with the value from before it, and those answers decrease as
 * the threads take them. So exactly one thread sees a value above zero that its
 * own cost takes to zero or past it, and that thread is the one that asks.
 */
void
emit_check (Instruction *at, Value *cost, GlobalVariable *counter,
            FunctionCallee promote, Constant *method)
{
	BasicBlock &block = *at->getParent ();
	Function &f = *block.getParent ();
	LLVMContext &ctx = f.getContext ();
	Type *i64 = Type::getInt64Ty (ctx);
	Constant *zero = ConstantInt::get (i64, 0);

	BasicBlock *done = block.splitBasicBlock (at->getIterator (), "tier_done");
	BasicBlock *count = BasicBlock::Create (ctx, "tier_count", &f, done);
	BasicBlock *ask = BasicBlock::Create (ctx, "tier_promote", &f, done);

	block.getTerminator ()->eraseFromParent ();

	IRBuilder<> at_block (&block);

	// The alignment is what make_counter () gives the global. This load and the
	// subtraction below each name one because the builder takes it, and an i64
	// atomic needs its natural alignment to stay a lock instruction.
	Value *left = at_block.CreateAlignedLoad (i64, counter, Align (8), "tier_left");

	at_block.CreateCondBr (at_block.CreateICmpSGT (left, zero), count, done);

	IRBuilder<> at_count (count);
	Value *before = at_count.CreateAtomicRMW (AtomicRMWInst::Sub, counter, cost,
	                                          Align (8), AtomicOrdering::Monotonic);
	Value *crossed = at_count.CreateAnd (at_count.CreateICmpSGT (before, zero),
	                                     at_count.CreateICmpSLE (before, cost));

	at_count.CreateCondBr (crossed, ask, done);

	IRBuilder<> at_ask (ask);
	CallInst *ask_call = at_ask.CreateCall (promote, { method });

	// The body runs after the request, so this call has to come back. LLVM
	// marks a call that reads none of the caller's frame as one that can
	// become a jump. A reader of that mark cannot tell it from a tail call
	// the method really made.
	ask_call->setTailCallKind (CallInst::TCK_NoTail);

	at_ask.CreateBr (done);
}

/// Takes the turns this exit's loops made off the counter.
///
/// The entry has charged the rest, so an exit a loop never ran before charges
/// nothing here.
void
emit_write_back (Instruction *at, AllocaInst *slot, GlobalVariable *counter,
                 FunctionCallee promote, Constant *method)
{
	Type *i64 = Type::getInt64Ty (at->getContext ());

	// In front of at, so the split inside emit_check () leaves the value where
	// it dominates the blocks that read it.
	IRBuilder<> at_point (at);
	Value *cost = at_point.CreateLoad (i64, slot, "tier_cost");

	emit_check (at, cost, counter, promote, method);
}

/// Collects the calls an exception can leave the frame through.
///
/// A call that already unwinds to a pad of the method's own is an invoke by now,
/// and a clause there decides what happens. This takes the plain calls, which are
/// the ones nothing in the body protects.
///
/// A tail call is left alone. An invoke is never a tail call, and the arch
/// lowering behind this pass reads the mark to keep the jump the IL asked for. A
/// noreturn call is left alone as well: it has already left the frame for good,
/// and collect_write_backs () charges in front of it.
void
collect_unwinding_calls (Function &f, SmallVectorImpl<CallInst *> &calls)
{
	for (Instruction &i : instructions (f)) {
		auto *call = dyn_cast<CallInst> (&i);

		if (call == nullptr || isa<IntrinsicInst> (call))
			continue;
		if (call->doesNotThrow () || call->doesNotReturn () || call->isInlineAsm ())
			continue;

		CallInst::TailCallKind kind = call->getTailCallKind ();

		if (kind == CallInst::TCK_Tail || kind == CallInst::TCK_MustTail)
			continue;

		calls.push_back (call);
	}
}

/// Makes the global that stands for the pad's clause in the exception tables.
///
/// eh-gather.cpp reads the kind word back and mono_lsda.cpp publishes one fault
/// clause over the whole body from it, so the clause index is unused.
GlobalVariable *
unwind_marker (Function &f)
{
	Module &m = *f.getParent ();
	Type *i32 = Type::getInt32Ty (m.getContext ());
	StructType *pair = StructType::get (i32, i32);
	std::string name = ("mono_tier_unwind_" + f.getName ()).str ();

	if (GlobalVariable *existing = m.getNamedGlobal (name))
		return existing;

	Constant *value = ConstantStruct::get (
		pair, { ConstantInt::get (i32, 0),
	                ConstantInt::get (i32, MONO_LSDA_KIND_TIER_UNWIND) });

	return new GlobalVariable (m, pair, /*isConstant=*/true,
	                           GlobalValue::PrivateLinkage, value, name);
}

/// Builds the pad a callee's exception reaches on its way through this frame,
/// and answers with it. The caller then sends each unwinding call to it.
///
/// The pad charges the turns the loops made, then calls mono_llvm_resume_unwind
/// (), the way any LLVM fault handler entered by unwinding does.
BasicBlock *
emit_unwind_pad (Function &f, AllocaInst *slot, GlobalVariable *counter,
                 FunctionCallee promote, Constant *method)
{
	Module &m = *f.getParent ();
	LLVMContext &ctx = f.getContext ();

	// The verifier refuses a landingpad in a function with no personality. A
	// method whose IL declared a clause was given the same one in
	// method-to-llvm.cpp.
	if (!f.hasPersonalityFn ())
		f.setPersonalityFn (cast<Constant> (
			m.getOrInsertFunction (
				 "mono_personality",
				 FunctionType::get (Type::getInt32Ty (ctx), true))
				.getCallee ()));

	BasicBlock *pad = BasicBlock::Create (ctx, "tier_unwind", &f);
	IRBuilder<> at_pad (pad);
	LandingPadInst *caught = at_pad.CreateLandingPad (
		StructType::get (PointerType::get (ctx, 0), at_pad.getInt32Ty ()), 1);

	caught->addClause (unwind_marker (f));

	FunctionCallee resume = m.getOrInsertFunction (
		"mono_llvm_resume_unwind", FunctionType::get (Type::getVoidTy (ctx), false));

	if (auto *fn = dyn_cast<Function> (resume.getCallee ()))
		fn->setDoesNotReturn ();

	CallInst *back = at_pad.CreateCall (resume);

	at_pad.CreateUnreachable ();

	// In front of the resume, so the check runs while the frame is still the
	// one that spent the turns. The split it makes leaves the landing pad
	// where it is, which is the first instruction of the block.
	emit_write_back (back, slot, counter, promote, method);
	return pad;
}

/// Takes the cost that does not depend on a loop off the counter, after the frame.
///
/// A static alloca has to stay in the entry block to be a stack slot, so the
/// check goes behind the last of them. Everything else carries on in the block
/// split off here.
///
/// The rest has to move even though it costs a block. Simplification runs ahead
/// of this pass, so by now the entry block can hold the whole body. Leaving that
/// in place puts the check between a musttail call and the ret it has to keep.
/// Codegen then refuses it with "failed to perform tail call elimination on a
/// call site marked musttail".
void
emit_entry_check (Function &f, uint64_t constant, GlobalVariable *counter,
                  FunctionCallee promote, Constant *method)
{
	BasicBlock &entry = f.getEntryBlock ();
	BasicBlock::iterator split = entry.getFirstNonPHIIt ();

	for (Instruction &i : entry)
		if (isa<AllocaInst> (&i))
			split = std::next (i.getIterator ());

	emit_check (&*split,
	            ConstantInt::get (Type::getInt64Ty (f.getContext ()), constant),
	            counter, promote, method);
}

/// Makes one counter, private so that the body reaches it PC-relative. An
/// external symbol puts a GOT load in front of every check instead.
GlobalVariable *
make_counter (Module &m, uint64_t threshold, StringRef name)
{
	Type *i64 = Type::getInt64Ty (m.getContext ());
	auto *counter =
		new GlobalVariable (m, i64, /*isConstant=*/false, GlobalValue::PrivateLinkage,
	                            ConstantInt::get (i64, threshold), name);

	counter->setAlignment (Align (8));
	return counter;
}

void
instrument (Function &f, uint64_t threshold, uint64_t entry_weight, Constant *method)
{
	Module &m = *f.getParent ();
	LLVMContext &ctx = m.getContext ();

	FunctionCallee promote = m.getOrInsertFunction (
		"mono_llvm_jit_tier2_promote",
		FunctionType::get (Type::getVoidTy (ctx), { PointerType::get (ctx, 0) },
	                           false));

	// The weights come off the body before this pass writes anything into it, so
	// a body does not count the cost of counting.
	DominatorTree dt (f);
	LoopInfo li (dt);
	DenseMap<const Loop *, uint64_t> per_loop;
	uint64_t sum = weigh (f, li, per_loop) + entry_weight;

	// The counter is signed. A cost past INT64_MAX reads as a negative number and
	// the subtraction adds to the counter, so such a body never promotes.
	uint64_t constant = sum > INT64_MAX ? INT64_MAX : sum;

	SmallVector<Instruction *, 8> points;
	SmallVector<CallInst *, 16> unwinding;

	collect_write_backs (f, points);
	collect_unwinding_calls (f, unwinding);

	GlobalVariable *counter = make_counter (m, threshold, "mono_tier_cost");

	/*
	 * A call of a body with no weight in any loop costs the constant and no
	 * more. A body that never returns, throws or unwinds has nowhere to write a
	 * running total back from, and the constant is the most it can be charged.
	 * Either way the entry charges the whole cost, so the body carries no
	 * accumulator.
	 *
	 * The entry block holds no loop header, because it has no predecessor and so
	 * lies on no cycle. So the split the check makes there leaves every header
	 * in place.
	 */
	if (per_loop.empty () || (points.empty () && unwinding.empty ())) {
		emit_entry_check (f, constant, counter, promote, method);
		return;
	}

	AllocaInst *slot = emit_accumulator (f, li, per_loop);

	for (Instruction *at : points)
		emit_write_back (at, slot, counter, promote, method);

	if (!unwinding.empty ()) {
		BasicBlock *pad = emit_unwind_pad (f, slot, counter, promote, method);

		// The calls were read off before any of the above, so what the
		// write-backs and the pad added is not turned into an invoke here.
		for (CallInst *call : unwinding)
			changeToInvokeAndSplitBasicBlock (call, pad);
	}

	// After the accumulator, because the slot has to stay in the entry block for
	// PromoteMemToReg and this check is what splits that block.
	emit_entry_check (f, constant, counter, promote, method);

	// A check splits a block, so the tree above no longer describes f.
	DominatorTree split (f);
	SmallVector<AllocaInst *, 1> slots = { slot };

	PromoteMemToReg (slots, split);
}

/// The number one attribute names, or zero where it names none.
///
/// Zero is a real value tier2_threshold () can return rather than a parse
/// failure, and a value nothing parses answers the same way.
uint64_t
threshold_from (const Function &f, StringRef name)
{
	uint64_t threshold = 0;

	if (!f.hasFnAttribute (name)
	    || f.getFnAttribute (name).getValueAsString ().getAsInteger (10, threshold))
		return 0;

	return threshold;
}

} // namespace

PreservedAnalyses
TierCounterPass::run (Module &m, ModuleAnalysisManager &)
{
	bool changed = false;

	for (Function &f : m) {
		if (f.isDeclaration () || !f.hasFnAttribute (tier_counter_attribute))
			continue;

		uint64_t threshold = threshold_from (f, tier_counter_attribute);

		if (threshold == 0)
			continue;

		// The translator recorded it, so the linker has an address for it. We
		// look the name up instead of inventing one: an invented name has
		// nothing behind it and links to zero.
		Constant *method = m.getNamedValue (
			f.getFnAttribute (tier_handle_attribute).getValueAsString ());

		if (method == nullptr)
			continue;

		instrument (f, threshold, threshold_from (f, tier_entry_weight_attribute),
		            method);
		changed = true;
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono
