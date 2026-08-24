/**
 * \file
 * \brief The two counters that ask for a body to be compiled again.
 */

#ifndef MONO_LLVM_PASSES_TIER_COUNTER_HPP
#define MONO_LLVM_PASSES_TIER_COUNTER_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

namespace mono {

/// How many calls the body takes before it asks for the next tier, as a decimal
/// string. A function without this attribute gets no counter at all.
///
/// tier2_threshold () (runtime/options.hpp) says what the number is and what
/// moves it.
constexpr llvm::StringRef tier_counter_attribute = "mono-tier-threshold";

/// How much work the body does before it asks for the next tier, as a decimal
/// string. Absent or zero leaves the body counting calls alone.
///
/// The unit is one instruction that emits code. tier2_cost_threshold ()
/// (runtime/options.hpp) says what the number is and what moves it.
constexpr llvm::StringRef tier_cost_attribute = "mono-tier-cost-threshold";

/// Names the symbol the promotion call-out is handed, which resolves to the
/// MonoDomainMethod record the body was compiled for.
constexpr llvm::StringRef tier_handle_attribute = "mono-tier-handle";

/// Makes each body that asks for one count its calls and the work it does, and
/// ask for the next tier when either count runs out.
///
/// Run this after the profile instrumentation, never before. Running it
/// first puts the counters' blocks into the CFG the instrumentation hashes.
/// The tier that reads the profile back carries no counter of its own, so
/// the hash never matches and the profile is dropped.
///
/// Run it after RestoreTailPositionPass as well. That pass looks for a call
/// whose block branches to a block that holds only a ret. A write-back in front
/// of that ret hides the shape, and the tail call is lost.
class TierCounterPass : public llvm::PassInfoMixin<TierCounterPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

} // namespace mono

#endif
