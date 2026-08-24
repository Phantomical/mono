/**
 * \file
 * \brief What a frame gives a stack object when the frame is never realigned.
 *
 * A function that hands a frame offset to something outside the frame declines
 * stack realignment, so its frame keeps the alignment the ABI gives it. A stack
 * object that asks for more than that is then placed at an offset which does not
 * hold its alignment, and codegen still reads the alignment off the IR. On amd64
 * that selects an aligned vector move, and the move faults on a frame that is 16
 * bytes aligned. The fault lands in the prologue, so the frame is not yet the
 * one its jit info describes.
 *
 * The claim is what is wrong rather than the placement, so this takes the claim
 * back down to what the frame gives.
 */

#ifndef MONO_LLVM_PASSES_CLAMP_FRAME_ALIGN_HPP
#define MONO_LLVM_PASSES_CLAMP_FRAME_ALIGN_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Lowers each alloca, and each memory operation that reaches one, to the stack
/// alignment the data layout names.
///
/// It acts only on a function that carries `no-realign-stack`. Run it after
/// every pass that can make an alloca or raise one's alignment, which puts it
/// last in the pipeline.
class ClampFrameAlignPass : public llvm::PassInfoMixin<ClampFrameAlignPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f,
	                             llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
