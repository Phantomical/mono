/**
 * \file
 * \brief Keeping a tier-1 profile counter out of the way of a null check.
 */

#ifndef MONO_LLVM_PASSES_PROFILE_COUNTER_SINK_HPP
#define MONO_LLVM_PASSES_PROFILE_COUNTER_SINK_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/**
 * Moves a profile counter below the dereference a null check folds into.
 *
 * LLVM's ImplicitNullChecks pass reads the arm a tagged check branches to when
 * the pointer is not null, looking for the memory operation to fold the test
 * into. It stops at the first instruction it cannot reorder. A counter update is
 * an atomicrmw, which is ordered, so an arm that opens with one keeps its check
 * as a compare and a branch.
 *
 * The counter is updated after that dereference rather than on entry to the arm.
 * A fault at the dereference therefore loses the count, which is what the profile
 * is willing to pay: the arm is entered once more than it says, and only along a
 * path that raises.
 *
 * Run behind InstrProfilingLoweringPass, which is what turns the increment
 * intrinsic into the atomicrmw.
 */
class ProfileCounterSinkPass : public llvm::PassInfoMixin<ProfileCounterSinkPass> {
public:
	llvm::PreservedAnalyses run (llvm::Function &f, llvm::FunctionAnalysisManager &fam);
};

} // namespace mono

#endif /* MONO_LLVM_PASSES_PROFILE_COUNTER_SINK_HPP */
