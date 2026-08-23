/**
 * \file
 * \brief Taking a counter update out of the loop it sits in.
 *
 * A counter incremented on every turn of a hot loop is a memory write per turn.
 * This pass accumulates the count in a register instead and writes it back once
 * at each exit from the loop.
 *
 * LLVM does the same thing inside its instrumentation lowering, over the loads
 * and stores that lowering writes. This one works on the increment intrinsics,
 * in front of it. That is what lets a counter be atomic and promoted at once.
 * The write-back is an increment intrinsic of its own, so the lowering still
 * decides how it is written.
 */

#ifndef MONO_LLVM_PASSES_PROFILE_COUNTER_PROMOTER_HPP
#define MONO_LLVM_PASSES_PROFILE_COUNTER_PROMOTER_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

struct PromotionPolicy {
	bool enabled = true;
	unsigned max_per_loop = 20;
	unsigned max_exiting = 8;
	bool skip_ret_exit_block = false;
	bool speculative_promotion_to_loop = false;
	bool iterative = true;

	static PromotionPolicy from_command_line ();
};

/// Must run after `PGOInstrumentationGen` and `LoopSimplifyPass`, and before
/// `InstrProfilingLoweringPass`.
class ProfileCounterPromoterPass : public llvm::PassInfoMixin<ProfileCounterPromoterPass> {
public:
	explicit ProfileCounterPromoterPass ();
	explicit ProfileCounterPromoterPass (PromotionPolicy policy);

	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);

private:
	PromotionPolicy policy_;
};

} // namespace mono

#endif
