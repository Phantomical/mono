/**
 * \file
 * \brief Putting a tail call's ret back where the optimizer moved it from.
 *
 * A call only becomes a jump if a `ret` follows it in the same block. The
 * marker on the call says the jump is wanted; tail position is what makes it
 * possible, and the backend checks for it rather than taking the marker's word.
 *
 * SimplifyCFG breaks that. A function with more than one `ret` - which is every
 * recursion with a base case, so every shape `tail.` exists for - has its
 * returning blocks tail-merged into one `common.ret` block reached by a branch.
 * The transform explicitly steps around a block ending in a musttail call,
 * because musttail's adjacency is a verifier rule it cannot break, but a plain
 * `tail` call carries no such protection: its block is merged like any other,
 * its `ret` becomes a `br`, and the marker quietly stops meaning anything. No
 * diagnostic is emitted, and the frame the prefix promised to hand away stays
 * on the stack.
 *
 * So this undoes that merge, for those blocks only. It is the inverse of
 * SimplifyCFG's performBlockTailMerging, run late and applied narrowly: a
 * branch to a merged return block is turned back into the `ret` it was, in
 * exactly the blocks whose last instruction is a call asking to be a jump.
 */

#ifndef MONO_LLVM_PASSES_RESTORE_TAIL_POSITION_HPP
#define MONO_LLVM_PASSES_RESTORE_TAIL_POSITION_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Restores tail position for calls marked as tail calls, undoing the return
/// merging that would otherwise leave the marker inert. Runs after the
/// simplification pipeline, since what it repairs is that pipeline's doing.
class RestoreTailPositionPass : public llvm::PassInfoMixin<RestoreTailPositionPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
