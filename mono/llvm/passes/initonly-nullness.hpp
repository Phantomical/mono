/**
 * \file
 * \brief Propagate the nullness of initialized readonly static references.
 *
 * Once the declaring class has finished initializing, a readonly static
 * reference cannot change between null and non-null. The collector may move
 * its target, but that does not change its nullness.
 */

#ifndef MONO_LLVM_PASSES_INITONLY_NULLNESS_HPP
#define MONO_LLVM_PASSES_INITONLY_NULLNESS_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

class InitonlyNullnessPass : public llvm::PassInfoMixin<InitonlyNullnessPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
