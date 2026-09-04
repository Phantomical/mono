/**
 * \file
 * \brief Delegate-fold and dispatch-guard, run off one memory solve.
 *
 * fold_delegate_invokes () and guard_dispatch_sites () (fold-delegate.hpp,
 * devirtualize.hpp) each read a MonoMemoryValues solve of their own where
 * run as separate passes. The pipeline never runs one without the other
 * beside it, so this fetches BlockFrequencyInfo and MonoMemoryValues once
 * and hands the same answer to both instead.
 */

#ifndef MONO_LLVM_PASSES_FOLD_DELEGATE_AND_GUARD_DISPATCH_HPP
#define MONO_LLVM_PASSES_FOLD_DELEGATE_AND_GUARD_DISPATCH_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Runs fold_delegate_invokes () and guard_dispatch_sites () off one
/// BlockFrequencyInfo and one MonoMemoryValues solve, read before either
/// half's own edits could put the answer out of date.
///
/// The two read disjoint IR shapes - a delegate Invoke's select-of-loads is
/// nothing like a builtin dispatch call - so what one folds changes nothing
/// the other reads out of MonoMemoryValues. What can go stale is a block's
/// BlockFrequencyInfo count where a delegate fold split it before this
/// round's guard reads it, which only feeds a guard's own branch weight,
/// not its correctness, and the next round's fresh solve settles it either
/// way: both run again every round buildTier2Pipeline () gives
/// TopDownInlinerPass.
class FoldDelegateAndGuardDispatchPass
	: public llvm::PassInfoMixin<FoldDelegateAndGuardDispatchPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif
