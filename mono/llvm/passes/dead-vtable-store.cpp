/**
 * \file
 * \brief Removes redundant stores of allocation vtables.
 */

#include "dead-vtable-store.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/ValueHandle.h>
#include <llvm/Transforms/Utils/Local.h>

using namespace llvm;

namespace mono {
namespace {

/// Returns the underlying pointer after stripping allocation ABI conversions.
///
/// Allocation lowering converts a vtable pointer with ptrtoint when the real
/// allocator accepts it as a native integer. Strip that conversion before
/// comparing it with the pointer stored by store_object_vtable ().
const Value *
strip_to_pointer (const Value *v)
{
	if (const auto *op = dyn_cast<Operator> (v))
		if (op->getOpcode () == Instruction::PtrToInt)
			v = op->getOperand (0);

	return v->stripPointerCasts ();
}

/// Returns whether \p store writes an allocation call's vtable argument into
/// the object returned by that call.
///
/// store_object_vtable () marks this write with `!invariant.group`. Also check
/// the operands so other invariant-group stores cannot match accidentally.
bool
is_allocation_vtable_store (StoreInst &store)
{
	if (!store.hasMetadata (LLVMContext::MD_invariant_group))
		return false;

	auto *call = dyn_cast<CallBase> (store.getPointerOperand ()->stripPointerCasts ());

	return call != nullptr && call->arg_size () != 0
	       && strip_to_pointer (call->getArgOperand (0))
			  == strip_to_pointer (store.getValueOperand ());
}

} // namespace

bool
erase_dead_vtable_stores (Function &f)
{
	SmallVector<StoreInst *, 8> dead;
	SmallVector<WeakTrackingVH, 8> potentially_dead;

	for (Instruction &in : instructions (f))
		if (auto *store = dyn_cast<StoreInst> (&in))
			if (is_allocation_vtable_store (*store))
				dead.push_back (store);

	for (StoreInst *store : dead) {
		if (auto *pointer = dyn_cast<Instruction> (store->getPointerOperand ()))
			potentially_dead.push_back (pointer);

		store->eraseFromParent ();
	}

	RecursivelyDeleteTriviallyDeadInstructionsPermissive (potentially_dead);

	return !dead.empty ();
}

PreservedAnalyses
EraseDeadVtableStorePass::run (Function &f, FunctionAnalysisManager &)
{
	if (!erase_dead_vtable_stores (f))
		return PreservedAnalyses::all ();

	PreservedAnalyses preserved;

	// Removing stores does not change the control-flow graph.
	preserved.preserveSet<CFGAnalyses> ();
	return preserved;
}

} // namespace mono
