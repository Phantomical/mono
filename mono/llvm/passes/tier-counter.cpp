#include "tier-counter.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

#include <cstdint>
#include <iterator>

using namespace llvm;

namespace mono {
namespace {

/*
 * A tier-1 body carries two counters and asks for tier 2 when either runs out.
 * One counts calls, at the entry. The other counts the work the body does, in
 * instructions that emit code, and the body adds that up in a register:
 *
 *         calls -= 1                          in the entry block
 *         acc = 0                             in the entry block
 *         acc += weight (loop)                in each loop header
 *         cost -= acc + acyclic_weight        at each exit
 *
 * Two counters, because each one alone leaves out a population that pays for a
 * tier-2 compile. A count of calls says nothing about how long a method runs, so
 * a method whose time is inside one loop never reaches it: euler keeps
 * Euler.Tunnel:calculateR () at tier 1 for a whole run that way. A count of work
 * says nothing about how often a method is called, so a body of three
 * instructions never reaches it however hot it is. SharpChess is full of those -
 * property getters called millions of times - and they are the methods whose
 * promotion pays there, because tier 2 folds them into their callers.
 *
 * Placement of the work count is per loop rather than per block, because no pass
 * behind this one tidies up what it writes, and tier-1 codegen is FastISel with
 * the fast register allocator. One add for each loop keeps the live range short
 * enough to stay in a register. A loop charges the weight of its own blocks once
 * for each turn, which is exact for a straight-line body and too much for a
 * branchy one. The blocks outside every loop are one constant, charged once for
 * each call.
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
/// Work is lost when the exception of a callee unwinds through this frame. No
/// instruction in the frame marks that point.
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

/// Puts the accumulator in, and answers with the weight of the blocks no loop
/// holds.
///
/// The accumulator is a stack slot, which PromoteMemToReg turns into phis once
/// the write-backs are in. Each store is the last value plus a step, so a slot
/// needs none of the bookkeeping an SSAUpdater wants.
/// ProfileCounterPromoterPass builds its own accumulators the same way.
///
/// Null when no loop in f has any weight, and the whole cost is then the
/// constant.
AllocaInst *
emit_accumulator (Function &f, LoopInfo &li, uint64_t &acyclic)
{
	DenseMap<const Loop *, uint64_t> per_loop;

	acyclic = 0;

	for (BasicBlock &block : f) {
		uint64_t weight = block_weight (block);

		if (weight == 0)
			continue;

		// The innermost loop, which is what makes the total the weight of the
		// blocks one loop holds and its sub-loops do not.
		if (const Loop *loop = li.getLoopFor (&block))
			per_loop[loop] += weight;
		else
			acyclic += weight;
	}

	if (per_loop.empty ())
		return nullptr;

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

	// The alignment is written out because the module has no layout of its own
	// yet: MonoJit::compile_batch () sets one after the pipeline has run, and
	// LLVM's default layout gives i64 an ABI alignment of 4.
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

/// Takes this exit's share of the work off the cost counter.
void
emit_write_back (Instruction *at, AllocaInst *slot, uint64_t acyclic,
                 GlobalVariable *counter, FunctionCallee promote, Constant *method)
{
	Type *i64 = Type::getInt64Ty (at->getContext ());
	Value *cost = ConstantInt::get (i64, acyclic);

	// In front of at, so the split inside emit_check () leaves the value where
	// it dominates the blocks that read it.
	if (slot != nullptr) {
		IRBuilder<> at_point (at);
		Value *acc = at_point.CreateLoad (i64, slot, "tier_acc");

		cost = acyclic == 0 ? acc : at_point.CreateAdd (acc, cost, "tier_cost");
	}

	emit_check (at, cost, counter, promote, method);
}

/// Takes one off the call counter, after the frame and in front of the work.
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
emit_entry_check (Function &f, GlobalVariable *counter, FunctionCallee promote,
                  Constant *method)
{
	BasicBlock &entry = f.getEntryBlock ();
	BasicBlock::iterator split = entry.getFirstNonPHIIt ();

	for (Instruction &i : entry)
		if (isa<AllocaInst> (&i))
			split = std::next (i.getIterator ());

	emit_check (&*split, ConstantInt::get (Type::getInt64Ty (f.getContext ()), 1),
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

bool
instrument (Function &f, uint64_t calls, uint64_t work, Constant *method)
{
	SmallVector<Instruction *, 8> points;

	collect_write_backs (f, points);

	// A body nothing leaves through a ret or a throw of its own has nowhere to
	// write a work count back from, so it counts calls alone.
	if (points.empty ())
		work = 0;

	if (calls == 0 && work == 0)
		return false;

	Module &m = *f.getParent ();
	LLVMContext &ctx = m.getContext ();

	FunctionCallee promote = m.getOrInsertFunction (
		"mono_llvm_jit_tier2_promote",
		FunctionType::get (Type::getVoidTy (ctx), { PointerType::get (ctx, 0) },
	                           false));

	/*
	 * The weights come off the body before this pass writes anything into it, so
	 * a body does not count the cost of counting. The accumulator goes in next,
	 * and the call check after it: the check splits the entry block, and the
	 * entry block holds no loop header, because it has no predecessor and so
	 * lies on no cycle. So the split leaves every header in place.
	 */
	DominatorTree dt (f);
	LoopInfo li (dt);
	uint64_t acyclic = 0;
	AllocaInst *slot = work == 0 ? nullptr : emit_accumulator (f, li, acyclic);

	/*
	 * A body with no loop does at most acyclic units in a call, because a call
	 * runs a subset of the blocks no loop holds. So it takes at least
	 * work / acyclic calls to spend the work counter, and where that is the call
	 * threshold or more the call check always gets there first. The work counter
	 * is then dead, and taking it out takes the accumulator and every write-back
	 * with it.
	 *
	 * This is most methods, and leaving it in is expensive: pystone under
	 * IronPython reads +8.9% of CPU with the write-backs in every body against
	 * +0.7% once only the bodies with a loop carry them.
	 */
	if (slot == nullptr && calls != 0 && acyclic <= work / calls)
		work = 0;

	if (calls != 0)
		emit_entry_check (f, make_counter (m, calls, "mono_tier_calls"), promote,
		                  method);

	if (work == 0)
		return true;

	GlobalVariable *counter = make_counter (m, work, "mono_tier_cost");

	for (Instruction *at : points)
		emit_write_back (at, slot, acyclic, counter, promote, method);

	if (slot != nullptr) {
		// A check splits a block, so the tree above no longer describes f.
		DominatorTree split (f);
		SmallVector<AllocaInst *, 1> slots = { slot };

		PromoteMemToReg (slots, split);
	}

	return true;
}

/// The threshold named by one attribute, or zero for a body that asks for no
/// check of that kind.
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

		uint64_t calls = threshold_from (f, tier_counter_attribute);
		uint64_t work = threshold_from (f, tier_cost_attribute);

		if (calls == 0 && work == 0)
			continue;

		// The translator recorded it, so the linker has an address for it. We
		// look the name up instead of inventing one: an invented name has
		// nothing behind it and links to zero.
		Constant *method = m.getNamedValue (
			f.getFnAttribute (tier_handle_attribute).getValueAsString ());

		if (method == nullptr)
			continue;

		changed |= instrument (f, calls, work, method);
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono
