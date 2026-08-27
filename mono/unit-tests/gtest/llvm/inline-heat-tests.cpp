/*
 * Tests for tier2_site_heat (), which ranks a call site inside the promoted
 * body around it, and for isColdCallSite () and getHotCallSiteThreshold (),
 * which ask it first.
 *
 * Pure LLVM: the answer reads a function attribute and the profile counts BFI
 * carries, so neither of these needs metadata or a runtime.
 *
 * The shapes and their counts are in ir/tier2-heat.ll, and that file says what
 * each body is for.
 */

#include "passes/inline-cost.hpp"
#include "passes/inline-policy.hpp"
#include "passes/tier-counter.hpp"

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

/*
 * ir/tier2-heat.ll, parsed, with the analyses the cost model reads over it.
 *
 * One module for each case rather than one for the file, because a case takes
 * an attribute off a body and the next one must not see that.
 */
struct HeatModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	PassBuilder pb;
	LoopAnalysisManager lam;
	FunctionAnalysisManager fam;
	CGSCCAnalysisManager cgam;
	ModuleAnalysisManager mam;

	HeatModule ()
	{
		SMDiagnostic problem;
		std::string path = std::string (MONO_LLVM_TESTS_IR_DIR) + "/tier2-heat.ll";

		module = parseAssemblyFile (path, problem, *context);

		if (module == nullptr) {
			std::string complaint;
			raw_string_ostream out (complaint);

			problem.print ("tier2-heat", out);
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

		EXPECT_NE (f, nullptr) << "ir/tier2-heat.ll has no " << name.str ();
		return *f;
	}

	BasicBlock &block (StringRef in, StringRef name)
	{
		for (BasicBlock &bb : function (in))
			if (bb.getName () == name)
				return bb;

		ADD_FAILURE () << in.str () << " has no block " << name.str ();
		return function (in).getEntryBlock ();
	}

	/// The one call in \p bb, which is what each case is about.
	CallBase &call_in (BasicBlock &bb)
	{
		for (Instruction &i : bb)
			if (auto *call = dyn_cast<CallBase> (&i))
				return *call;

		ADD_FAILURE () << bb.getName ().str () << " holds no call";
		return *static_cast<CallBase *> (nullptr);
	}

	/// How often the profile counted that block.
	uint64_t count_of (StringRef in, StringRef name)
	{
		Function &root = function (in);

		return fam.getResult<BlockFrequencyAnalysis> (root)
			.getBlockProfileCount (&block (in, name))
			.value_or (0);
	}

	/// What the ranking says about the call in block \p name of function \p in.
	std::optional<SiteHeat> heat_of (StringRef in, StringRef name)
	{
		Function &root = function (in);

		return tier2_site_heat (call_in (block (in, name)),
		                        &fam.getResult<BlockFrequencyAnalysis> (root));
	}

	/// The budget the cost model weighed that same call against.
	int threshold_of (StringRef in, StringRef name)
	{
		Function &callee = function ("callee");
		auto get_ac = [&] (Function &f) -> AssumptionCache & {
			return fam.getResult<AssumptionAnalysis> (f);
		};
		auto get_tli = [&] (Function &f) -> const TargetLibraryInfo & {
			return fam.getResult<TargetLibraryAnalysis> (f);
		};
		auto get_bfi = [&] (Function &f) -> BlockFrequencyInfo & {
			return fam.getResult<BlockFrequencyAnalysis> (f);
		};
		ProfileSummaryInfo &psi = mam.getResult<ProfileSummaryAnalysis> (*module);

		return mono::getInlineCost (call_in (block (in, name)), &callee,
		                            mono::getInlineParams (),
		                            fam.getResult<TargetIRAnalysis> (callee),
		                            get_ac, get_tli, get_bfi, &psi)
			.getThreshold ();
	}
};

/*
 * The counts every case below rests on. A weight in the file that stopped
 * giving these would leave each case testing a ratio of its own. The verdict
 * would still come out the same, but for the wrong reason.
 */
TEST (InlineHeat, TheFileLeavesTheCountsItsShapesName)
{
	HeatModule m;

	EXPECT_EQ (m.count_of ("looping", "entry"), 1000u);
	EXPECT_EQ (m.count_of ("looping", "body"), 4096000u);
	EXPECT_EQ (m.count_of ("loopless", "entry"), 1000u);
	EXPECT_EQ (m.count_of ("rare_under", "rare"), 19u);
	EXPECT_EQ (m.count_of ("rare_at", "rare"), 20u);
}

TEST (InlineHeat, TheEntryBlockOfALoopingBodyIsOrdinary)
{
	HeatModule m;

	EXPECT_EQ (m.heat_of ("looping", "entry"), SiteHeat::ordinary);
}

TEST (InlineHeat, TheLoopOfALoopingBodyIsHot)
{
	HeatModule m;

	EXPECT_EQ (m.heat_of ("looping", "body"), SiteHeat::hot);
}

/*
 * Every block of a body with no loop runs each time it is entered, so nothing
 * in it stands out as the work and every site in it is hot.
 */
TEST (InlineHeat, EveryBlockOfALooplessBodyIsHot)
{
	HeatModule m;

	EXPECT_EQ (m.heat_of ("loopless", "entry"), SiteHeat::hot);
}

/*
 * The cold share is the whole of what separates these two, so they sit either
 * side of it: 19 of every 1000 entries is under two percent and 20 is not.
 */
TEST (InlineHeat, ABlockUnderTheColdShareIsCold)
{
	HeatModule m;

	EXPECT_EQ (m.heat_of ("rare_under", "rare"), SiteHeat::cold);
}

TEST (InlineHeat, ABlockAtTheColdShareIsOrdinary)
{
	HeatModule m;

	EXPECT_EQ (m.heat_of ("rare_at", "rare"), SiteHeat::ordinary);
}

TEST (InlineHeat, ABodyWithNoCounterIsNotRanked)
{
	HeatModule m;

	m.function ("looping").removeFnAttr (tier_counter_attribute);

	EXPECT_EQ (m.heat_of ("looping", "entry"), std::nullopt);
}

TEST (InlineHeat, ABodyWithNoCountsIsNotRanked)
{
	HeatModule m;

	// The entry count rides on the function as !prof, and a body whose counts
	// the profile reader dropped arrives without it.
	m.function ("looping").setMetadata (LLVMContext::MD_prof, nullptr);

	EXPECT_EQ (m.heat_of ("looping", "entry"), std::nullopt);
}

/*
 * What isColdCallSite () and getHotCallSiteThreshold () decide. A verdict above
 * passes whether or not those calls are there, so the budget is read here as
 * well.
 */
TEST (InlineHeat, TheEntryBlockOfAPromotedBodyGetsTheDefaultBudget)
{
	HeatModule m;

	EXPECT_EQ (m.threshold_of ("looping", "entry"), 225);
}

/*
 * The same site with the counter taken off, which is the degeneracy this
 * answers. The summary's cold percentile lands on the body's own entry count,
 * so the site the body always runs reads cold and is clamped.
 */
TEST (InlineHeat, WithoutTheCounterTheSummaryClampsTheEntryBlock)
{
	HeatModule m;

	m.function ("looping").removeFnAttr (tier_counter_attribute);

	EXPECT_EQ (m.threshold_of ("looping", "entry"), 45);
}

} // namespace
} // namespace test
} // namespace mono
