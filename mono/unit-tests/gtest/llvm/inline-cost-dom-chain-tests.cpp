/*
 * Tests for two gaps in CallAnalyzer::visitCmpInst (). Both are about a
 * call whose operand a mono fold already substituted: a null check several
 * one-predecessor blocks above the guard that needs it, and a formal
 * argument's own call-site attribute losing to a caller-side substitution
 * that cannot answer.
 *
 * Pure LLVM: CallAnalyzer::analyze ()'s own formal-argument mapping
 * (SimplifiedValues[&FAI] = *CAI) substitutes a caller's operand for a bare
 * argument, the same way folded_type_test () substitutes one for a call.
 * ir/inline-cost-dom-chain.ll reaches the same code through that mapping,
 * with no mono fold and no runtime under it.
 */

#include "passes/inline-cost.hpp"

#include <llvm/AsmParser/Parser.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/Analysis/ProfileSummaryInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// ir/inline-cost-dom-chain.ll, parsed, with the analyses getInlineCost ()
/// reads over it.
struct DomChainModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	PassBuilder pb;
	LoopAnalysisManager lam;
	FunctionAnalysisManager fam;
	CGSCCAnalysisManager cgam;
	ModuleAnalysisManager mam;

	DomChainModule ()
	{
		SMDiagnostic problem;
		std::string path =
			std::string (MONO_LLVM_TESTS_IR_DIR) + "/inline-cost-dom-chain.ll";

		module = parseAssemblyFile (path, problem, *context);

		if (module == nullptr) {
			std::string complaint;
			raw_string_ostream out (complaint);

			problem.print ("inline-cost-dom-chain", out);
			ADD_FAILURE () << complaint;
			return;
		}

		std::string complaint;
		raw_string_ostream out (complaint);

		EXPECT_FALSE (verifyModule (*module, &out)) << complaint;

		pb.registerModuleAnalyses (mam);
		pb.registerCGSCCAnalyses (cgam);
		pb.registerFunctionAnalyses (fam);
		pb.registerLoopAnalyses (lam);
		pb.crossRegisterProxies (lam, fam, cgam, mam);
	}

	Function &function (StringRef name)
	{
		Function *f = module->getFunction (name);

		EXPECT_NE (f, nullptr) << "ir/inline-cost-dom-chain.ll has no " << name.str ();
		return *f;
	}

	/// \p root's own call to \p callee, wherever in its entry block it sits.
	CallBase &call_to (StringRef root, StringRef callee)
	{
		Function &want = function (callee);

		for (Instruction &i : function (root).getEntryBlock ())
			if (auto *call = dyn_cast<CallBase> (&i))
				if (call->getCalledFunction () == &want)
					return *call;

		report_fatal_error (Twine (root) + " holds no call to " + callee);
	}

	/// What the cost model priced \p root's call to \p callee at.
	int cost_of (StringRef root, StringRef callee)
	{
		Function &f = function (callee);
		auto get_ac = [&] (Function &fn) -> AssumptionCache & {
			return fam.getResult<AssumptionAnalysis> (fn);
		};
		auto get_tli = [&] (Function &fn) -> const TargetLibraryInfo & {
			return fam.getResult<TargetLibraryAnalysis> (fn);
		};
		auto get_bfi = [&] (Function &fn) -> BlockFrequencyInfo & {
			return fam.getResult<BlockFrequencyAnalysis> (fn);
		};
		ProfileSummaryInfo &psi = mam.getResult<ProfileSummaryAnalysis> (*module);

		return mono::getInlineCost (call_to (root, callee), &f, mono::getInlineParams (),
		                            fam.getResult<TargetIRAnalysis> (f),
		                            get_ac, get_tli, get_bfi, &psi)
			.getCost ();
	}
};

/*
 * @dominated and @undominated differ only in the null check standing above
 * two unrelated tests and the guard that matters. The gap this guards: the
 * guard's own comparison never folded, so the number for @dominated was
 * never lower regardless of what stood above it.
 */
TEST (InlineCostDomChain, ANullCheckSeveralBlocksUpFoldsTheGuardBelowIt)
{
	DomChainModule m;

	EXPECT_LT (m.cost_of ("root_dominated", "dominated"),
	          m.cost_of ("root_undominated", "undominated"));
}

/*
 * The exact numbers the inequality above reads. A change that narrows the
 * gap for the wrong reason -- pricing @dominated's dead arm back in, say --
 * still fails here even if it leaves the inequality standing.
 */
TEST (InlineCostDomChain, TheDominatingCheckPricesOutExactlyTheDeadArm)
{
	DomChainModule m;

	EXPECT_EQ (m.cost_of ("root_dominated", "dominated"), 75);
	EXPECT_EQ (m.cost_of ("root_undominated", "undominated"), 165);
}

/*
 * raw_operand ()'s own null check answers off %o's call-site attribute
 * directly. Settled resolves %o to %arbitrary instead, which carries no
 * attribute or alloca of its own. A fix that only tried the substituted
 * Value would leave this guard unfolded, the same price
 * root_raw_operand_unproven's call reads with no attribute anywhere to try.
 */
TEST (InlineCostDomChain, AFormalArgumentsOwnAttributeAnswersDespiteAnUnprovableSubstitution)
{
	DomChainModule m;

	EXPECT_LT (m.cost_of ("root_raw_operand", "raw_operand"),
	          m.cost_of ("root_raw_operand_unproven", "raw_operand"));
}

} // namespace
} // namespace test
} // namespace mono
