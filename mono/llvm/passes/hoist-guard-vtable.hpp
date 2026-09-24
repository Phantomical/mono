#ifndef MONO_LLVM_PASSES_HOIST_GUARD_VTABLE_HPP
#define MONO_LLVM_PASSES_HOIST_GUARD_VTABLE_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/* Hoist guard vtable comparisons for loop-invariant receivers. */
class HoistGuardVtablePass : public llvm::PassInfoMixin<HoistGuardVtablePass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
