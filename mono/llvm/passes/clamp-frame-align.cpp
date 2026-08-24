#include "clamp-frame-align.hpp"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {

PreservedAnalyses
ClampFrameAlignPass::run (Function &f, FunctionAnalysisManager &)
{
	if (!f.hasFnAttribute ("no-realign-stack"))
		return PreservedAnalyses::all ();

	MaybeAlign stack = f.getParent ()->getDataLayout ().getStackAlignment ();

	// A data layout with no stack alignment in it says nothing about what a
	// frame gives, and this pass has no other source for that number.
	if (!stack)
		return PreservedAnalyses::all ();

	SmallPtrSet<const Value *, 8> lowered;

	for (Instruction &i : instructions (f)) {
		auto *slot = dyn_cast<AllocaInst> (&i);

		if (slot == nullptr || slot->getAlign () <= *stack)
			continue;

		slot->setAlignment (*stack);
		lowered.insert (slot);
	}

	if (lowered.empty ())
		return PreservedAnalyses::all ();

	// An operation keeps its own alignment, which the placement no longer
	// holds either. getUnderlyingObject () walks the offsets a slice of the
	// object is reached through.
	auto reaches_a_lowered_slot = [&] (const Value *pointer) {
		return lowered.contains (getUnderlyingObject (pointer));
	};

	for (Instruction &i : instructions (f)) {
		if (auto *load = dyn_cast<LoadInst> (&i)) {
			if (load->getAlign () > *stack
			    && reaches_a_lowered_slot (load->getPointerOperand ()))
				load->setAlignment (*stack);
			continue;
		}

		if (auto *store = dyn_cast<StoreInst> (&i)) {
			if (store->getAlign () > *stack
			    && reaches_a_lowered_slot (store->getPointerOperand ()))
				store->setAlignment (*stack);
			continue;
		}

		auto *block = dyn_cast<MemIntrinsic> (&i);

		if (block == nullptr)
			continue;

		MaybeAlign destination = block->getDestAlign ();

		if (destination && *destination > *stack
		    && reaches_a_lowered_slot (block->getRawDest ()))
			block->setDestAlignment (*stack);

		auto *copy = dyn_cast<MemTransferInst> (block);

		if (copy == nullptr)
			continue;

		MaybeAlign source = copy->getSourceAlign ();

		if (source && *source > *stack
		    && reaches_a_lowered_slot (copy->getRawSource ()))
			copy->setSourceAlignment (*stack);
	}

	return PreservedAnalyses::all ();
}

} // namespace mono
