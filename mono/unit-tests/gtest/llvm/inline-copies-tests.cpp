/*
 * Tests for StripInlineCopiesPass, which takes back the bodies an inliner
 * translated in beside a caller and did not fold.
 *
 * Pure LLVM: the pass names no metadata, so neither do these.
 */

#include "passes/inline-copies.hpp"

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

/// A module holding a caller, and a copy of `published` it calls under the
/// placeholder name the translator gave it.
struct CopyModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	Function *copy = nullptr;

	CopyModule (StringRef placeholder, StringRef published)
	{
		module = std::make_unique<Module> ("copies", *context);

		Type *i32 = Type::getInt32Ty (*context);
		FunctionType *type = FunctionType::get (i32, { i32 }, false);

		copy = Function::Create (type, GlobalValue::ExternalLinkage, placeholder,
		                         module.get ());

		IRBuilder<> in_copy (BasicBlock::Create (*context, "entry", copy));

		in_copy.CreateRet (in_copy.CreateAdd (copy->getArg (0),
		                                      ConstantInt::get (i32, 1)));
		mark_inline_copy (*copy, published);

		caller = Function::Create (type, GlobalValue::ExternalLinkage, "caller",
		                           module.get ());

		IRBuilder<> in_caller (BasicBlock::Create (*context, "entry", caller));

		in_caller.CreateRet (in_caller.CreateCall (copy, { caller->getArg (0) }));
	}

	void strip ()
	{
		ModuleAnalysisManager mam;
		PassBuilder pb;

		pb.registerModuleAnalyses (mam);

		FunctionAnalysisManager fam;
		LoopAnalysisManager lam;
		CGSCCAnalysisManager cgam;

		pb.registerFunctionAnalyses (fam);
		pb.registerLoopAnalyses (lam);
		pb.registerCGSCCAnalyses (cgam);
		pb.crossRegisterProxies (lam, fam, cgam, mam);

		StripInlineCopiesPass ().run (*module, mam);
	}

	/// The function the caller's one call goes to.
	Function *called () const
	{
		for (BasicBlock &bb : *caller)
			for (Instruction &i : bb)
				if (auto *call = dyn_cast<CallInst> (&i))
					return call->getCalledFunction ();

		return nullptr;
	}
};

} // namespace

TEST (InlineCopies, ACopyGoesBackToADeclarationOfThePublishedEntry)
{
	CopyModule m ("placeholder@0x1234", "Some.Class:Method@0x1234");

	m.strip ();

	Function *reached = m.called ();

	ASSERT_NE (reached, nullptr);
	EXPECT_EQ (reached->getName (), "Some.Class:Method@0x1234");
	EXPECT_TRUE (reached->isDeclaration ());
	// A declaration with local linkage does not verify, and one that kept
	// alwaysinline reads to an inliner as a body it can fold.
	EXPECT_EQ (reached->getLinkage (), GlobalValue::ExternalLinkage);
	EXPECT_FALSE (reached->hasFnAttribute (Attribute::AlwaysInline));
	EXPECT_FALSE (reached->hasFnAttribute (inline_copy_attribute));
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

/*
 * A translation that called the same method after the copy was made declared it
 * under the published name, because the copy answered to the placeholder. Both
 * references have to end up on one value.
 */
TEST (InlineCopies, ACopyMergesIntoADeclarationThatAlreadyHasItsName)
{
	CopyModule m ("placeholder@0x1234", "Some.Class:Method@0x1234");
	Function *declared =
		Function::Create (m.copy->getFunctionType (), GlobalValue::ExternalLinkage,
	                          "Some.Class:Method@0x1234", m.module.get ());

	m.strip ();

	EXPECT_EQ (m.called (), declared);
	EXPECT_EQ (m.module->getFunction ("placeholder@0x1234"), nullptr);
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

TEST (InlineCopies, AModuleWithNoCopiesIsLeftAlone)
{
	CopyModule m ("placeholder@0x1234", "Some.Class:Method@0x1234");

	m.strip ();
	EXPECT_FALSE (errorToBool (inline_copies_stripped (*m.module)));

	// The assertion behind the sweep only fires on a copy the sweep never saw.
	m.copy->addFnAttr (inline_copy_attribute, "Some.Class:Method@0x1234");
	EXPECT_TRUE (errorToBool (inline_copies_stripped (*m.module)));
}

} // namespace test
} // namespace mono
