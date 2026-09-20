/**
 * \file
 * \brief Removes redundant stores of allocation vtables.
 */

#ifndef MONO_LLVM_PASSES_DEAD_VTABLE_STORE_HPP
#define MONO_LLVM_PASSES_DEAD_VTABLE_STORE_HPP

#include <llvm/IR/PassManager.h>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

/// Removes stores that copy an allocator's vtable argument into the header of
/// the object it returned. Returns whether the function changed.
///
/// This must run after lower_allocations (), when the allocation call has been
/// replaced with the real allocator that initializes the object header.
bool erase_dead_vtable_stores (llvm::Function &f);

/// Removes redundant allocation vtable stores from a function.
class EraseDeadVtableStorePass : public llvm::PassInfoMixin<EraseDeadVtableStorePass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
