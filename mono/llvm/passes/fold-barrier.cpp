/**
 * \file
 * \brief Erasing a write barrier whose destination is a local, and taking a
 * value copy apart where the IR says an open copy is safe.
 */

#include "fold-barrier.hpp"

#include "builtins.hpp"
#include "gc-barrier.hpp"

#include "../runtime/options.hpp"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

using namespace llvm;

namespace mono {

bool
fold_stack_barriers (Function &f)
{
	bool changed = false;

	for (CallBase *site : builtin_sites (f, gc_barrier_name)) {
		// The declaration is nounwind, so every site is a call.
		auto *call = dyn_cast<CallInst> (site);

		if (call == nullptr)
			continue;

		if (!points_to_the_frame (call->getArgOperand (0)))
			continue;

		// The call names the destination, so the alloca escapes and SROA
		// promotes none of it. Erasing the call is what lets a local holding a
		// reference reach a register, and the store beside it go with it.
		call->eraseFromParent ();
		changed = true;
	}

	return changed;
}

bool
open_value_copies (Function &f)
{
	Module &m = *f.getParent ();
	bool changed = false;

	if (m.getFunction (gc_value_copy_name) == nullptr)
		return false;

	for (CallBase *site : builtin_sites (f, gc_value_copy_name)) {
		// The declaration is nounwind, so every site is a call.
		auto *call = dyn_cast<CallInst> (site);

		if (call == nullptr)
			continue;

		Value *dest = call->getArgOperand (0);

		/*
		 * A destination in the frame owes no card, so the whole call becomes the
		 * copy it was made of. The collector scans a frame as a root at each
		 * collection, and a card only ever records a reference in the heap.
		 *
		 * A destination in the heap keeps the call. The copy and the cards have
		 * to reach the collector together, which is what the icall behind this
		 * builtin does and what a copy standing in the open cannot.
		 */
		if (!points_to_the_frame (dest))
			continue;

		IRBuilder<> b (call);

		b.SetCurrentDebugLocation (call->getDebugLoc ());

		Value *src = call->getArgOperand (1);
		Value *count = b.CreateZExt (call->getArgOperand (2), b.getInt64Ty ());
		Value *bytes = b.CreateMul (count, call->getArgOperand (3));
		Align dest_align = call->getParamAlign (0).valueOrOne ();
		Align src_align = call->getParamAlign (1).valueOrOne ();

		if (call->hasFnAttr (gc_no_overlap_attr))
			b.CreateMemCpy (dest, dest_align, src, src_align, bytes);
		else
			b.CreateMemMove (dest, dest_align, src, src_align, bytes);

		call->eraseFromParent ();
		changed = true;
	}

	return changed;
}

} // namespace mono
