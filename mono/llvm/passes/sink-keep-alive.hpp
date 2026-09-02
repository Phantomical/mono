/**
 * \file
 * \brief Moving a delegate keep_alive () marker out of the loop it pins.
 *
 * `keep_alive ()` (`method-to-llvm/call.cpp`) is inline asm, and
 * `LoopVectorizationLegality::canVectorizeInstrs ()` refuses to vectorize any
 * loop that holds one. Where a marker's delegate stays one object every turn,
 * a single marker after the loop keeps it rooted for exactly as long. The
 * loop is then left without a call blocking the vectorizer.
 */

#ifndef MONO_LLVM_PASSES_SINK_KEEP_ALIVE_HPP
#define MONO_LLVM_PASSES_SINK_KEEP_ALIVE_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Sinks a keep_alive () marker to the outermost loop where its delegate is
/// still loop-invariant. Every copy inside that loop is replaced with one at
/// each of the loop's exit blocks.
///
/// A loop that never leaves has no exit block, so its markers stay put:
/// there is nowhere outside it to hold the delegate.
class SinkKeepAlivePass : public llvm::PassInfoMixin<SinkKeepAlivePass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
