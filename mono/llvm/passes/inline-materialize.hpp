/**
 * \file
 * \brief Shared helpers for materializing and checking inline candidates.
 */

#ifndef MONO_LLVM_PASSES_INLINE_MATERIALIZE_HPP
#define MONO_LLVM_PASSES_INLINE_MATERIALIZE_HPP

#include "top-down-inline.hpp"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/IR/PassManager.h>

#include <optional>

namespace llvm {
class AssumptionCache;
class CallBase;
class Function;
class Module;
class PassBuilder;
} // namespace llvm

namespace mono {

class OneFileFS;

/// Analysis managers for preparing a candidate in a scratch module.
struct ScratchAnalyses {
	llvm::LoopAnalysisManager lam;
	llvm::FunctionAnalysisManager fam;
	llvm::CGSCCAnalysisManager cgam;
	llvm::ModuleAnalysisManager mam;

	explicit ScratchAnalyses (llvm::PassBuilder &pb);

	void clear ();
};

/// Materialize and prepare a candidate, replacing its declaration in \p m.
/// Returns null if translation or linking fails.
llvm::Function *materialize_candidate (llvm::Module &m, llvm::Function &decl,
                                       InlineCandidates &candidates,
                                       llvm::ModulePassManager &prepare, ScratchAnalyses &scratch,
                                       OneFileFS &profile_fs, std::optional<SiteHeat> heat,
                                       const llvm::CallBase &call);

/// Whether the callee contains its own EH clause or finally marker.
bool has_own_clause (const llvm::Function &callee);

/// Whether all callee landing-pad clauses are supported by EH merging.
bool mergeable_clause_kinds_only (const llvm::Function &callee);

/// Whether a trial inline leaves one of \p callee's EH markers live.
bool clause_survives_inline (llvm::CallBase &call, llvm::Function &callee,
                             llvm::FunctionPassManager &simplify,
                             llvm::FunctionAnalysisManager &fam,
                             llvm::function_ref<llvm::AssumptionCache &(llvm::Function &)> get_ac);

} // namespace mono

#endif
