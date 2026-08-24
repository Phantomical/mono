/*
 * Tests for ProtectedNullChecksPass, which takes the !make.implicit tag off the
 * null checks of a method that has EH clauses.
 *
 * Pure LLVM: the pass names no metadata, so neither do these.
 */

#include "passes/protected-null-checks.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>

#include <gtest/gtest.h>

#include <memory>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// A module holding one function shaped like a null check: a test of the
/// argument, a cold arm that returns zero, and a load in the other arm.
struct CheckModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *function = nullptr;
	BranchInst *branch = nullptr;

	explicit CheckModule (bool has_clauses)
	{
		module = std::make_unique<Module> ("checks", *context);

		Type *i32 = Type::getInt32Ty (*context);
		Type *ptr = PointerType::getUnqual (*context);
		FunctionType *type = FunctionType::get (i32, { ptr }, false);

		function = Function::Create (type, GlobalValue::ExternalLinkage, "check",
		                             module.get ());

		if (has_clauses)
			function->addFnAttr ("mono-has-eh-clauses");

		BasicBlock *entry = BasicBlock::Create (*context, "entry", function);
		BasicBlock *null = BasicBlock::Create (*context, "null", function);
		BasicBlock *not_null = BasicBlock::Create (*context, "not_null", function);

		IRBuilder<> in_entry (entry);

		branch = in_entry.CreateCondBr (in_entry.CreateIsNull (function->getArg (0)),
		                                null, not_null);
		branch->setMetadata (make_implicit_metadata, MDNode::get (*context, {}));

		IRBuilder<> in_null (null);

		in_null.CreateRet (ConstantInt::get (i32, 0));

		IRBuilder<> in_not_null (not_null);

		in_not_null.CreateRet (in_not_null.CreateLoad (i32, function->getArg (0)));
	}

	bool tagged () const
	{
		return branch->getMetadata (make_implicit_metadata) != nullptr;
	}

	void strip ()
	{
		FunctionAnalysisManager fam;

		ProtectedNullChecksPass ().run (*function, fam);
	}
};

TEST (ProtectedNullChecks, AMethodWithNoClausesKeepsTheTag)
{
	CheckModule m (/*has_clauses=*/false);

	ASSERT_TRUE (m.tagged ());
	m.strip ();
	EXPECT_TRUE (m.tagged ());
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

TEST (ProtectedNullChecks, AMethodWithClausesLosesTheTag)
{
	CheckModule m (/*has_clauses=*/true);

	ASSERT_TRUE (m.tagged ());
	m.strip ();
	EXPECT_FALSE (m.tagged ());
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

} // namespace
} // namespace test
} // namespace mono
