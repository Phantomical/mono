/**
 * \file
 * \brief Giving FastISel an `unreachable` it selects.
 */

#ifndef MONO_LLVM_PASSES_TRAP_UNREACHABLE_HPP
#define MONO_LLVM_PASSES_TRAP_UNREACHABLE_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Writes a `llvm.trap` call in front of each `unreachable` that lowers to a trap.
///
/// Tier 1 only. FastISel gives a block up to SelectionDAG on the `ISD::TRAP` an
/// `unreachable` lowers to, and selects the intrinsic call itself. The code is
/// the same `ud2`, because `UnreachableInst::shouldLowerToTrap ()` declines for
/// an `unreachable` a trap already stands in front of.
///
/// Run it behind the simplification pipeline, which takes the call back out.
class TrapUnreachablePass : public llvm::PassInfoMixin<TrapUnreachablePass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
