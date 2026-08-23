/**
 * \file
 * \brief Putting a tail call's ret back where the optimizer moved it from.
 *
 * LLVM can only turn a `tail call` into a jump if it is immediately followed
 * by a `ret`. The SimplifyCFG pass merges all the `ret`s into one block,
 * which breaks this. We still want tail calls to happen, so this pass
 * undoes the merge for function calls.
 */

#ifndef MONO_LLVM_PASSES_RESTORE_TAIL_POSITION_HPP
#define MONO_LLVM_PASSES_RESTORE_TAIL_POSITION_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Puts back the `ret` a merge replaced with a branch, in a block that ends
/// in a call marked to become a jump.
///
/// Run this after the simplification pipeline. That pipeline is what merges
/// the returns.
class RestoreTailPositionPass : public llvm::PassInfoMixin<RestoreTailPositionPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
