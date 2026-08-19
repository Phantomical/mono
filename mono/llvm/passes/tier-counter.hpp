/**
 * \file
 * \brief The entry counter that asks for a body to be compiled again.
 */

#ifndef MONO_LLVM_PASSES_TIER_COUNTER_HPP
#define MONO_LLVM_PASSES_TIER_COUNTER_HPP

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/PassManager.h>

namespace mono {

/// How many calls the body takes before it asks for the next tier, as a decimal
/// string. A function without this attribute gets no counter.
constexpr llvm::StringRef tier_counter_attribute = "mono-tier-threshold";

/// Names the symbol the promotion call-out is handed, which resolves to the
/// MonoMethod the body is for. The translator records it, so the pass only has
/// to find it.
constexpr llvm::StringRef tier_handle_attribute = "mono-tier-handle";

/// Puts a counted entry check in front of every body that asks for one.
///
/// Run this after the profile instrumentation, never before. The blocks it adds
/// would otherwise be part of the CFG the instrumentation hashes, and the tier
/// that reads the profile back carries no counter - so its hash would never
/// match and every profile would be dropped.
class TierCounterPass : public llvm::PassInfoMixin<TierCounterPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

} // namespace mono

#endif
