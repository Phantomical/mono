#include "tier-counter.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <iterator>

using namespace llvm;

namespace mono {
namespace {

/*
 * if (counter > 0)
 *         if (--counter == 0)
 *                 promote (method);
 *
 * The plain load in front keeps the atomic off the path a body takes once it
 * has been counted. That is every call but the first few.
 */
void
emit_counter (Function &f, uint32_t threshold, Constant *method)
{
	Module &m = *f.getParent ();
	LLVMContext &ctx = m.getContext ();
	Type *i32 = Type::getInt32Ty (ctx);

	// Private, so the body reaches it PC-relative. An external symbol puts a
	// GOT load in front of every call instead.
	auto *counter = new GlobalVariable (m, i32, /*isConstant=*/false,
	                                    GlobalValue::PrivateLinkage,
	                                    ConstantInt::get (i32, threshold),
	                                    "mono_tier_counter");
	counter->setAlignment (Align (4));

	FunctionCallee promote = m.getOrInsertFunction (
		"mono_llvm_jit_tier2_promote",
		FunctionType::get (Type::getVoidTy (ctx), { PointerType::get (ctx, 0) },
	                           false));

	BasicBlock &entry = f.getEntryBlock ();

	/*
	 * After the frame and in front of the work. A static alloca has to stay in
	 * the entry block to be a stack slot, so the check goes behind the last of
	 * them, and everything else carries on in the block split off here.
	 *
	 * The rest has to move even though it costs a block. Simplification runs
	 * ahead of this pass, so by now the entry block can hold the whole body.
	 * Leaving that in place puts the check between a musttail call and the ret
	 * it has to keep, which codegen refuses with "failed to perform tail call
	 * elimination on a call site marked musttail".
	 */
	BasicBlock::iterator split = entry.getFirstNonPHIIt ();

	for (Instruction &i : entry) {
		if (isa<AllocaInst> (&i))
			split = std::next (i.getIterator ());
	}

	BasicBlock *body = entry.splitBasicBlock (split, "tier_body");
	BasicBlock *count = BasicBlock::Create (ctx, "tier_count", &f, body);
	BasicBlock *ask = BasicBlock::Create (ctx, "tier_promote", &f, body);

	entry.getTerminator ()->eraseFromParent ();

	IRBuilder<> at_entry (&entry);
	Value *left = at_entry.CreateLoad (i32, counter, "tier_left");

	at_entry.CreateCondBr (at_entry.CreateICmpSGT (left, ConstantInt::get (i32, 0)),
	                       count, body);

	IRBuilder<> at_count (count);
	// atomicrmw returns the value from before it, so the decrement that
	// lands on zero is the one that saw a one.
	Value *before = at_count.CreateAtomicRMW (AtomicRMWInst::Sub, counter,
	                                          ConstantInt::get (i32, 1), MaybeAlign (),
	                                          AtomicOrdering::Monotonic);

	at_count.CreateCondBr (at_count.CreateICmpEQ (before, ConstantInt::get (i32, 1)),
	                       ask, body);

	IRBuilder<> at_ask (ask);
	CallInst *ask_call = at_ask.CreateCall (promote, { method });

	// The body runs after the request, so this call has to come back. LLVM
	// marks a call that reads none of the caller's frame as one that can become
	// a jump, and a reader of that mark cannot tell it from a tail call the
	// method really made.
	ask_call->setTailCallKind (CallInst::TCK_NoTail);

	at_ask.CreateBr (body);
}

} // namespace

PreservedAnalyses
TierCounterPass::run (Module &m, ModuleAnalysisManager &)
{
	bool changed = false;

	for (Function &f : m) {
		if (f.isDeclaration () || !f.hasFnAttribute (tier_counter_attribute))
			continue;

		uint32_t threshold = 0;

		if (f.getFnAttribute (tier_counter_attribute)
		            .getValueAsString ()
		            .getAsInteger (10, threshold)
		    || threshold == 0)
			continue;

		// The translator recorded it, so the linker has an address for it. We
		// look the name up instead of inventing one: an invented name has
		// nothing behind it and links to zero.
		Constant *method = m.getNamedValue (
			f.getFnAttribute (tier_handle_attribute).getValueAsString ());

		if (method == nullptr)
			continue;

		emit_counter (f, threshold, method);
		changed = true;
	}

	return changed ? PreservedAnalyses::none () : PreservedAnalyses::all ();
}

} // namespace mono
