/**
 * \file
 * \brief Dropping a generic-context fetch that a dominating fetch already did.
 *
 * The translator emits a fetch where the IL names the metadata. A body that
 * names one class or one field several times therefore gets a fetch for each
 * mention. The translator cannot see far enough to know which of them comes
 * first. This pass, which can, leaves one and hands its value to the rest.
 */

#ifndef MONO_LLVM_PASSES_RGCTX_DEDUP_HPP
#define MONO_LLVM_PASSES_RGCTX_DEDUP_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Replaces a fetch of a generic-context slot with the result of a fetch of the
/// same slot that dominates it.
///
/// Run this before RgctxFetchPass, while each fetch is still one call. The
/// guard that pass builds is a diamond, and the load in it is not the value a
/// later fetch can take.
class RgctxDedupPass : public llvm::PassInfoMixin<RgctxDedupPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f,
	                             llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
