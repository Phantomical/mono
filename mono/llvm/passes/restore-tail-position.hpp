/**
 * \file
 * \brief Putting a tail call's ret back where the optimizer moved it from.
 *
 * LLVM turns a call into a jump only when a `ret` follows it in the same
 * block. The marker on the call asks for the jump, and codegen then looks
 * for that `ret` itself instead of taking the marker's word.
 *
 * A function with more than one `ret` - which is every recursion with a base
 * case - is what SimplifyCFG tail-merges. The returning blocks become one
 * `common.ret` block that each of them branches to. SimplifyCFG steps around
 * a block that ends in a `musttail` call, because the adjacency there is a
 * verifier rule. A plain `tail` call has no such protection: its `ret`
 * becomes a `br` and the marker stops meaning anything. LLVM prints no
 * diagnostic, and the frame the `tail.` prefix promised to hand away stays
 * on the stack.
 *
 * So this pass undoes that merge, in the blocks whose last instruction is a
 * call that asks to be a jump.
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
