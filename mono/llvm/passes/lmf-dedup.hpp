/** Reuse LMF register captures across sequential pushes. */

#ifndef MONO_LLVM_PASSES_LMF_DEDUP_HPP
#define MONO_LLVM_PASSES_LMF_DEDUP_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Reuses an earlier slot when its pop dominates a later push.
class LmfDedupPass : public llvm::PassInfoMixin<LmfDedupPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
