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
 * in front of it. That is what lets a counter be atomic and promoted at once:
 * the write-back is an increment intrinsic of its own, so the lowering still
 * decides how it is written.
 */

#ifndef MONO_LLVM_PASSES_PROFILE_COUNTER_PROMOTER_HPP
#define MONO_LLVM_PASSES_PROFILE_COUNTER_PROMOTER_HPP

#include <llvm/IR/PassManager.h>

namespace mono {

/// Settings for ProfileCounterPromoterPass.
///
/// Each field below is the default of the `mono-` option that sets it. A
/// default-constructed policy and the one from_command_line () returns agree
/// until a command line moves one.
struct PromotionPolicy {
	/// Whether to promote at all.
	bool enabled = true;

	/// The maximum number of counter variables that can be promoted in any
	/// one loop.
	unsigned max_per_loop = 20;

	/// How many loop-exit blocks are allowed before we give up on promoting
	/// counter writes out of the loop.
	///
	/// We need to write all the relevant counters in each loop exit, so this
	/// option exists to prevent code size blowup in that case.
	unsigned max_exiting = 8;

	/// If true, we skip loops that contain function exits.
	bool skip_ret_exit_block = false;

	/// Allow promoting a counter out of a nested loop even if there isn't
	/// capacity for it in the parent loop.
	bool speculative_promotion_to_loop = false;

	/// Once a counter is promoted out of a nested loop can it then continue
	/// to be promoted out of parent loops?
	bool iterative = true;

	/// Returns the policy the command line describes.
	static PromotionPolicy from_command_line ();
};

/// Hoist counter updates out of loops.
///
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
