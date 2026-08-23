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
/// MonoDomainMethod record the body was compiled for.
constexpr llvm::StringRef tier_handle_attribute = "mono-tier-handle";

/// Puts a counted entry check in front of every body that asks for one.
///
/// Run this after the profile instrumentation, never before. Running it
/// first puts the counter's blocks into the CFG the instrumentation hashes.
/// The tier that reads the profile back carries no counter of its own, so
/// the hash never matches and the profile is dropped.
class TierCounterPass : public llvm::PassInfoMixin<TierCounterPass> {
public:
	llvm::PreservedAnalyses run (llvm::Module &m, llvm::ModuleAnalysisManager &mam);
};

} // namespace mono

#endif
