/**
 * \file
 * \brief Erasing an allocation nothing reads, with the stores that fill the
 * object in and the write barriers those stores asked for.
 */

#ifndef MONO_LLVM_PASSES_DEAD_ALLOC_HPP
#define MONO_LLVM_PASSES_DEAD_ALLOC_HPP

#include <llvm/IR/PassManager.h>

namespace llvm {
class Function;
} // namespace llvm

namespace mono {

/// Erases each allocation in \p f that the function fills in and never reads,
/// and says whether it changed anything.
///
/// The allocation, its field stores and the barriers beside those stores go in
/// one step, so the object never exists and owes no card. A barrier erased on
/// its own leaves a store the collector has no card for.
///
/// The `.kept` forms are not read, so those objects are kept regardless.
///
/// Run it in front of the `LowerStage::post_optimization` lowering. That
/// lowering writes a barrier back as a compare and a byte store, which this
/// walk cannot tell from the program's own code.
bool erase_dead_allocations (llvm::Function &f);

/// Runs erase_dead_allocations () over one function.
class EraseDeadAllocationsPass : public llvm::PassInfoMixin<EraseDeadAllocationsPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
