/**
 * \file
 * \brief Erasing a write barrier whose destination is a local.
 */

#include "fold-barrier.hpp"

#include "builtins.hpp"
#include "gc-barrier.hpp"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

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

		// getUnderlyingObject () stops at a phi and at a select, so a
		// destination that is one of two objects reads as neither.
		if (!isa<AllocaInst> (getUnderlyingObject (call->getArgOperand (0))))
			continue;

		// The call names the destination, so the alloca escapes and SROA
		// promotes none of it. Erasing the call is what lets a local holding a
		// reference reach a register, and the store beside it go with it.
		call->eraseFromParent ();
		changed = true;
	}

	return changed;
}

} // namespace mono
