/**
 * \file
 * \brief Guard class-init calls that remain after elision.
 */

#ifndef MONO_LLVM_PASSES_CLASS_INIT_GUARD_HPP
#define MONO_LLVM_PASSES_CLASS_INIT_GUARD_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Guard each remaining class-init call with a vtable initialization check.
class ClassInitGuardPass : public llvm::PassInfoMixin<ClassInitGuardPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
