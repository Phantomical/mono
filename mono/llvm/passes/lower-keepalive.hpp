/**
 * \file
 * \brief Giving FastISel a keep_alive () marker it does not drop.
 *
 * `keep_alive ()` (`method-to-llvm/call.cpp`) writes `llvm.fake.use`, and
 * FastISel lowers the intrinsic to nothing at all rather than to a real use
 * of its argument. Tier 1 selects with FastISel, so a tier-1 body needs the
 * marker rewritten into a shape that selector does keep live before it
 * reaches codegen.
 */

#ifndef MONO_LLVM_PASSES_LOWER_KEEPALIVE_HPP
#define MONO_LLVM_PASSES_LOWER_KEEPALIVE_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Rewrites every `llvm.fake.use` call in a function into the empty,
/// register-constrained inline asm read that FastISel does keep live.
///
/// Tier 1 only. Tier 2 selects with SelectionDAG, which lowers
/// `llvm.fake.use` to a real `FAKE_USE` machine instruction on its own.
class LowerKeepAlivePass : public llvm::PassInfoMixin<LowerKeepAlivePass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
