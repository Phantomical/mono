/*
 * Tests for ProfileCounterSinkPass, which moves a profile counter below the
 * dereference a null check folds into.
 *
 * Pure LLVM: the pass includes no mono header, so neither do these.
 */

#include "passes/profile-counter-sink.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/ProfileData/InstrProf.h>

#include <gtest/gtest.h>

#include <memory>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/**
 * A function shaped like a translated null check: the entry tests the argument
 * against null and branches to a throw arm, and the other arm counts itself and
 * then reads through the argument.
 */
struct CheckedFunction {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *function = nullptr;
	BasicBlock *not_null = nullptr;
	AtomicRMWInst *counter = nullptr;
	LoadInst *load = nullptr;

	/// \param counter_name the counter array the update addresses. The pass only
	/// moves an update that reaches one of the instrumentation's own arrays.
	explicit CheckedFunction (StringRef counter_name, bool tagged = true)
	{
		module = std::make_unique<Module> ("checks", *context);

		Type *i32 = Type::getInt32Ty (*context);
		Type *i64 = Type::getInt64Ty (*context);
		PointerType *ptr = PointerType::get (*context, 0);
		FunctionType *type = FunctionType::get (i32, { ptr }, false);

		function = Function::Create (type, GlobalValue::ExternalLinkage, "checked",
		                             module.get ());

		auto *counters = new GlobalVariable (*module, i64, /*isConstant=*/false,
		                                     GlobalValue::PrivateLinkage,
		                                     ConstantInt::get (i64, 0), counter_name);

		BasicBlock *entry = BasicBlock::Create (*context, "entry", function);
		BasicBlock *throw_arm = BasicBlock::Create (*context, "throw", function);

		not_null = BasicBlock::Create (*context, "no_throw", function);

		IRBuilder<> at_entry (entry);
		BranchInst *branch = at_entry.CreateCondBr (
			at_entry.CreateIsNull (function->getArg (0)), throw_arm, not_null);

		if (tagged)
			branch->setMetadata (LLVMContext::MD_make_implicit,
			                     MDNode::get (*context, {}));

		IRBuilder<> at_throw (throw_arm);

		at_throw.CreateRet (ConstantInt::get (i32, 0));

		IRBuilder<> at_arm (not_null);

		counter = at_arm.CreateAtomicRMW (AtomicRMWInst::Add, counters,
		                                  ConstantInt::get (i64, 1), MaybeAlign (),
		                                  AtomicOrdering::Monotonic);
		load = at_arm.CreateLoad (
			i32, at_arm.CreateConstInBoundsGEP1_64 (Type::getInt8Ty (*context),
		                                                function->getArg (0), 24));
		at_arm.CreateRet (load);
	}

	/// Runs the pass and returns whether the counter now stands behind the read.
	bool sinks ()
	{
		PassBuilder pb;
		FunctionAnalysisManager fam;

		pb.registerFunctionAnalyses (fam);
		ProfileCounterSinkPass ().run (*function, fam);

		EXPECT_FALSE (verifyFunction (*function, &errs ()));

		for (Instruction &i : *not_null) {
			if (&i == counter)
				return false;
			if (&i == load)
				return true;
		}

		return false;
	}
};

std::string
profile_counter_name ()
{
	return (getInstrProfCountersVarPrefix () + "checked").str ();
}

TEST (ProfileCounterSink, ACounterMovesBelowTheRead)
{
	CheckedFunction checked (profile_counter_name ());

	EXPECT_TRUE (checked.sinks ());
}

TEST (ProfileCounterSink, AnUntaggedBranchKeepsItsCounterInPlace)
{
	CheckedFunction checked (profile_counter_name (), /*tagged=*/false);

	EXPECT_FALSE (checked.sinks ());
}

TEST (ProfileCounterSink, AnAtomicOnSomethingElseStaysInPlace)
{
	CheckedFunction checked ("an_ordinary_global");

	EXPECT_FALSE (checked.sinks ());
}

} // namespace
} // namespace test
} // namespace mono
