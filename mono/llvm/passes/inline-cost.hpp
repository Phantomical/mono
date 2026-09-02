/**
 * \file
 * \brief Pricing a call site, off a copy of LLVM's own cost model.
 */

#ifndef MONO_LLVM_PASSES_INLINE_COST_HPP
#define MONO_LLVM_PASSES_INLINE_COST_HPP

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/Analysis/InlineCost.h>

#include <optional>

namespace llvm {
class AssumptionCache;
class BlockFrequencyInfo;
class CallBase;
class DataLayout;
class EphemeralValuesCache;
class Function;
class OptimizationRemarkEmitter;
class ProfileSummaryInfo;
class TargetLibraryInfo;
class TargetTransformInfo;
} // namespace llvm

/// A copy of the cost model in llvm/lib/Analysis/InlineCost.cpp. CallAnalyzer is
/// in no header, so a copy is what lets the model read what the managed metadata
/// knows about a call site.
///
/// Each function below answers what the LLVM function of the same name answers,
/// off the same defaults. The options those defaults come from carry a `mono-`
/// prefix, because LLVM's CommandLine calls report_fatal_error () on a name
/// registered twice. So `--llvm-opt=-mono-inline-threshold=N` tunes this copy,
/// and `-inline-threshold=N` tunes the copy the rest of the pipeline reads.
///
/// Call each of these qualified. The arguments are llvm types, so an unqualified
/// call from inside `mono` finds LLVM's overload through argument-dependent
/// lookup beside this one, and the two are ambiguous.
namespace mono {

class ConstantValues;

llvm::InlineCost getInlineCost(
    llvm::CallBase &Call, const llvm::InlineParams &Params,
    llvm::TargetTransformInfo &CalleeTTI,
    llvm::function_ref<llvm::AssumptionCache &(llvm::Function &)> GetAssumptionCache,
    llvm::function_ref<const llvm::TargetLibraryInfo &(llvm::Function &)> GetTLI,
    llvm::function_ref<llvm::BlockFrequencyInfo &(llvm::Function &)> GetBFI = nullptr,
    llvm::ProfileSummaryInfo *PSI = nullptr,
    llvm::OptimizationRemarkEmitter *ORE = nullptr,
    llvm::function_ref<llvm::EphemeralValuesCache &(llvm::Function &)> GetEphValuesCache =
        nullptr,
    llvm::function_ref<ConstantValues &(llvm::Function &)> GetConstantValues =
        nullptr);

llvm::InlineCost getInlineCost(
    llvm::CallBase &Call, llvm::Function *Callee, const llvm::InlineParams &Params,
    llvm::TargetTransformInfo &CalleeTTI,
    llvm::function_ref<llvm::AssumptionCache &(llvm::Function &)> GetAssumptionCache,
    llvm::function_ref<const llvm::TargetLibraryInfo &(llvm::Function &)> GetTLI,
    llvm::function_ref<llvm::BlockFrequencyInfo &(llvm::Function &)> GetBFI = nullptr,
    llvm::ProfileSummaryInfo *PSI = nullptr,
    llvm::OptimizationRemarkEmitter *ORE = nullptr,
    llvm::function_ref<llvm::EphemeralValuesCache &(llvm::Function &)> GetEphValuesCache =
        nullptr,
    llvm::function_ref<ConstantValues &(llvm::Function &)> GetConstantValues =
        nullptr);

std::optional<llvm::InlineResult> getAttributeBasedInliningDecision(
    llvm::CallBase &Call, llvm::Function *Callee, llvm::TargetTransformInfo &CalleeTTI,
    llvm::function_ref<const llvm::TargetLibraryInfo &(llvm::Function &)> GetTLI);

std::optional<int> getInliningCostEstimate(
    llvm::CallBase &Call, llvm::TargetTransformInfo &CalleeTTI,
    llvm::function_ref<llvm::AssumptionCache &(llvm::Function &)> GetAssumptionCache,
    llvm::function_ref<llvm::BlockFrequencyInfo &(llvm::Function &)> GetBFI = nullptr,
    llvm::function_ref<const llvm::TargetLibraryInfo &(llvm::Function &)> GetTLI = nullptr,
    llvm::ProfileSummaryInfo *PSI = nullptr,
    llvm::OptimizationRemarkEmitter *ORE = nullptr);

std::optional<llvm::InlineCostFeatures> getInliningCostFeatures(
    llvm::CallBase &Call, llvm::TargetTransformInfo &CalleeTTI,
    llvm::function_ref<llvm::AssumptionCache &(llvm::Function &)> GetAssumptionCache,
    llvm::function_ref<llvm::BlockFrequencyInfo &(llvm::Function &)> GetBFI = nullptr,
    llvm::function_ref<const llvm::TargetLibraryInfo &(llvm::Function &)> GetTLI = nullptr,
    llvm::ProfileSummaryInfo *PSI = nullptr,
    llvm::OptimizationRemarkEmitter *ORE = nullptr);

llvm::InlineResult isInlineViable (llvm::Function &Callee);

int getCallsiteCost (const llvm::TargetTransformInfo &TTI, const llvm::CallBase &Call,
                     const llvm::DataLayout &DL);

llvm::InlineParams getInlineParams ();
llvm::InlineParams getInlineParams (int Threshold);
llvm::InlineParams getInlineParams (unsigned OptLevel, unsigned SizeOptLevel);

} // namespace mono

#endif
