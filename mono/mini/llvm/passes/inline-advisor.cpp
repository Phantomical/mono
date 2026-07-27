/**
 * \file
 * inline-advisor.cpp - see inline-advisor.hpp for what this is for.
 *
 * The advisor is LLVM's DefaultInlineAdvisor with one change: before asking
 * getInlineCost () about a call site, it raises the threshold by what the cost
 * model is about to charge for the callee's runtime-check failure blocks.
 *
 * A null check translates to
 *
 *     br i1 %isnull, label %EX_BB, label %NOEX_BB, !make.implicit !0
 *   EX_BB:
 *     call void @llvm_throw_corlib_exception_abs_trampoline (i32 329), !mono.runtime-check !1
 *     unreachable
 *
 * and a bounds check to the same thing without the metadata on the branch.
 * Either way the failure block is code that never runs, ends in `unreachable`
 * and gets laid out away from everything else - but the cost model prices its
 * throw as an ordinary call, ~35, because nothing in the IR says otherwise.
 * LLVM does draw this distinction when it has a profile (costBenefitAnalysis ()
 * subtracts the size of never-executed blocks); a JIT with no profile has to
 * say it some other way, and `mono.runtime-check` on the call is that.
 *
 * The credit is a threshold bump rather than a cost exclusion because the cost
 * side is not reachable: the charge happens inside InlineCostCallAnalyzer,
 * which stock LLVM does not export.
 */

#include "inline-advisor.hpp"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/Analysis/InlineAdvisor.h>
#include <llvm/Analysis/InlineCost.h>
#include <llvm/Analysis/OptimizationRemarkEmitter.h>
#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <memory>

using namespace llvm;

namespace mono {

namespace {

/* What emit_cond_system_exception () tags a check's throw call with. */
const char kRuntimeCheckTag[] = "mono.runtime-check";

/*
 * InlineCostCallAnalyzer's own per-call charge: one instruction per argument
 * for the setup, the target's call penalty, and the instruction itself.
 * CallPenalty is a cl::opt private to InlineCost.cpp, so 25 - its default - is
 * spelled out here; the TTI hook is the one part of it a target can override.
 */
int
call_cost (const CallBase &call, const TargetTransformInfo &tti)
{
	const int instr = InlineConstants::getInstrCost ();
	const unsigned penalty = tti.getInlineCallPenalty (call.getCaller (), call, 25);

	return (int) call.arg_size () * instr + (int) penalty + instr;
}

/*
 * What the cost model will charge for BB, if BB is a block that only exists to
 * fail a runtime check. Zero for anything else - in particular for anything
 * that can fall through to code that runs, which is what makes crediting it
 * back safe.
 */
int
runtime_check_block_cost (const BasicBlock &bb, const TargetTransformInfo &tti)
{
	if (!isa<UnreachableInst> (bb.getTerminator ()))
		return 0;

	int cost = 0;
	bool is_check = false;

	for (const Instruction &inst : bb) {
		if (const auto *call = dyn_cast<CallBase> (&inst)) {
			is_check |= call->getMetadata (kRuntimeCheckTag) != nullptr;
			cost += call_cost (*call, tti);
		} else if (!isa<UnreachableInst> (&inst)) {
			cost += InlineConstants::getInstrCost ();
		}
	}

	return is_check ? cost : 0;
}

/*
 * The threshold credit for CALLEE: what its check-failure blocks are about to
 * be charged.
 */
int
runtime_check_credit (const Function &callee, const TargetTransformInfo &tti)
{
	int credit = 0;

	for (const BasicBlock &bb : callee)
		credit += runtime_check_block_cost (bb, tti);

	return credit;
}

class MonoInlineAdvisor : public InlineAdvisor {
public:
	MonoInlineAdvisor (Module &m, FunctionAnalysisManager &fam, InlineParams params,
	                   InlineContext ic)
	    : InlineAdvisor (m, fam, ic), params_ (params)
	{
	}

private:
	std::unique_ptr<InlineAdvice> getAdviceImpl (CallBase &cb) override;

	InlineParams params_;
};

std::unique_ptr<InlineAdvice>
MonoInlineAdvisor::getAdviceImpl (CallBase &cb)
{
	Function &caller = *cb.getCaller ();
	auto &ore = FAM.getResult<OptimizationRemarkEmitterAnalysis> (caller);

	/* Cached rather than computed: there is no profile in a JIT compile. */
	ProfileSummaryInfo *psi =
		FAM.getResult<ModuleAnalysisManagerFunctionProxy> (caller)
			.getCachedResult<ProfileSummaryAnalysis> (*caller.getParent ());

	auto get_assumption_cache = [&] (Function &f) -> AssumptionCache & {
		return FAM.getResult<AssumptionAnalysis> (f);
	};
	auto get_bfi = [&] (Function &f) -> BlockFrequencyInfo & {
		return FAM.getResult<BlockFrequencyAnalysis> (f);
	};
	auto get_tli = [&] (Function &f) -> const TargetLibraryInfo & {
		return FAM.getResult<TargetLibraryAnalysis> (f);
	};

	auto get_inline_cost = [&] (CallBase &call) {
		Function &callee = *call.getCalledFunction ();
		auto &callee_tti = FAM.getResult<TargetIRAnalysis> (callee);

		InlineParams params = params_;
		if (!callee.isDeclaration ())
			params.DefaultThreshold += runtime_check_credit (callee, callee_tti);

		return getInlineCost (call, params, callee_tti, get_assumption_cache, get_tli,
		                      get_bfi, psi, nullptr);
	};

	auto cost = shouldInline (cb, get_inline_cost, ore, params_.EnableDeferral.value_or (true));

	return std::make_unique<InlineAdvice> (this, cb, ore, cost.has_value ());
}

InlineAdvisor *
advisor_factory (Module &m, FunctionAnalysisManager &fam, InlineParams params, InlineContext ic)
{
	return new MonoInlineAdvisor (m, fam, params, ic);
}

} // namespace

void
register_mono_inline_advisor (ModuleAnalysisManager &mam)
{
	mam.registerPass ([] { return PluginInlineAdvisorAnalysis (advisor_factory); });
}

} // namespace mono
