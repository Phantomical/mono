/**
 * \file
 * \brief EliminateDelegateInvokesPass on its own.
 *
 * `eliminate_delegate_invokes ()` (`passes/builtins.hpp`) is what does the work.
 * `EliminateDelegateAndGuardDispatchPass`
 * (`eliminate-delegate-and-guard-dispatch.hpp`) is what the pipeline runs
 * instead.
 */

#ifndef MONO_LLVM_PASSES_ELIMINATE_DELEGATE_HPP
#define MONO_LLVM_PASSES_ELIMINATE_DELEGATE_HPP

#include <llvm/IR/PassManager.h>

namespace llvm {
class Function;
}

namespace mono {

/// Fetches BlockFrequencyInfo and a MonoMemoryValues solve through \p fam and
/// calls eliminate_delegate_invokes ().
class EliminateDelegateInvokesPass : public llvm::PassInfoMixin<EliminateDelegateInvokesPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
