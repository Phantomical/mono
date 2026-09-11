/**
 * \file
 * \brief Dropping a finally's thread-abort bookkeeping once its body has
 * nothing left in it to protect.
 */

#ifndef MONO_LLVM_PASSES_ELIMINATE_EMPTY_FINALLY_HPP
#define MONO_LLVM_PASSES_ELIMINATE_EMPTY_FINALLY_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Erases a finally's body markers, and the thread-abort deferral built
/// around them, once nothing survives between them but the markers
/// themselves.
///
/// Run this behind the simplification pipeline that gives a finally's own
/// effects their chance to prove dead.
class EliminateEmptyFinallyPass : public llvm::PassInfoMixin<EliminateEmptyFinallyPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
