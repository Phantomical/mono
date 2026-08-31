/*
 * Tests for save_lmf_cost (), which prices a callee whose front end pushes an
 * LMF entry, and for its wiring into InlineCostCallAnalyzer::onAnalysisStart ().
 *
 * Pure LLVM: the answer reads a function attribute, so neither of these needs
 * metadata or a runtime.
 */

#include "passes/inline-cost.hpp"
#include "passes/inline-policy.hpp"

#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

#include <gtest/gtest.h>

#include <memory>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// A caller with one call to `callee`, both taking and returning i32.
struct CallModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *callee = nullptr;
	CallBase *call = nullptr;

	CallModule ()
	{
		module = std::make_unique<Module> ("save-lmf", *context);

		Type *i32 = Type::getInt32Ty (*context);
		FunctionType *type = FunctionType::get (i32, { i32 }, false);

		callee = Function::Create (type, GlobalValue::ExternalLinkage, "callee",
		                           module.get ());

		IRBuilder<> in_callee (BasicBlock::Create (*context, "entry", callee));

		in_callee.CreateRet (in_callee.CreateAdd (callee->getArg (0),
		                                          ConstantInt::get (i32, 1)));

		Function *caller = Function::Create (type, GlobalValue::ExternalLinkage,
		                                     "caller", module.get ());

		IRBuilder<> in_caller (BasicBlock::Create (*context, "entry", caller));

		call = in_caller.CreateCall (callee, { caller->getArg (0) });
		in_caller.CreateRet (call);

		EXPECT_FALSE (verifyModule (*module, &errs ()));
	}

	/// What getInlineCost () charges for folding `callee` at `call`.
	int cost ()
	{
		PassBuilder pb;
		LoopAnalysisManager lam;
		FunctionAnalysisManager fam;
		CGSCCAnalysisManager cgam;
		ModuleAnalysisManager mam;

		pb.registerModuleAnalyses (mam);
		pb.registerCGSCCAnalyses (cgam);
		pb.registerFunctionAnalyses (fam);
		pb.registerLoopAnalyses (lam);
		pb.crossRegisterProxies (lam, fam, cgam, mam);

		auto get_ac = [&] (Function &f) -> AssumptionCache & {
			return fam.getResult<AssumptionAnalysis> (f);
		};
		auto get_tli = [&] (Function &f) -> const TargetLibraryInfo & {
			return fam.getResult<TargetLibraryAnalysis> (f);
		};

		return mono::getInlineCost (*call, callee, mono::getInlineParams (),
		                            fam.getResult<TargetIRAnalysis> (*callee),
		                            get_ac, get_tli)
			.getCost ();
	}
};

} // namespace

TEST (InlineSaveLmf, AFunctionWithNoAttributeCostsNothing)
{
	LLVMContext context;
	Module module ("save-lmf", context);
	Function *f = Function::Create (FunctionType::get (Type::getVoidTy (context), false),
	                                GlobalValue::ExternalLinkage, "plain", &module);

	EXPECT_EQ (save_lmf_cost (*f), 0);
}

TEST (InlineSaveLmf, AFunctionWithTheAttributeCostsThePenalty)
{
	LLVMContext context;
	Module module ("save-lmf", context);
	Function *f = Function::Create (FunctionType::get (Type::getVoidTy (context), false),
	                                GlobalValue::ExternalLinkage, "wrapper", &module);

	f->addFnAttr (save_lmf_attribute);

	EXPECT_GT (save_lmf_cost (*f), 0);
}

// onAnalysisStart () adds save_lmf_cost () once, beside the coldcc penalty, so
// tagging the callee has to move the reported cost by exactly that amount.
TEST (InlineSaveLmf, TaggingTheCalleeMovesGetInlineCostByThePenalty)
{
	CallModule m;
	int untagged = m.cost ();

	m.callee->addFnAttr (save_lmf_attribute);

	EXPECT_EQ (m.cost (), untagged + save_lmf_cost (*m.callee));
}

} // namespace test
} // namespace mono
