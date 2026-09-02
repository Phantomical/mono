/*
 * Tests for rebuild_isinst_over_incoming () and isinst_settles_over_incoming (),
 * which answer an isinst one PHI edge at a time where the merge as a whole
 * disagrees.
 *
 * Pure LLVM. Both take a plain callback instead of a MonoConstantValues walk,
 * so a fake answer can stand in for cast_answer () with no class or domain
 * behind it.
 */

#include "passes/fold-cast.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

#include <gtest/gtest.h>

#include <memory>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// A function with two predecessor blocks, each returning a value of its own,
/// merged into one phi in a third.
struct MergeModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *f = nullptr;
	PHINode *phi = nullptr;
	Value *left = nullptr;
	Value *right = nullptr;

	MergeModule ()
	{
		module = std::make_unique<Module> ("merge", *context);

		Type *ptr = PointerType::get (*context, 0);
		Function *make = Function::Create (FunctionType::get (ptr, false),
		                                   GlobalValue::ExternalLinkage, "make",
		                                   module.get ());

		f = Function::Create (FunctionType::get (ptr, { Type::getInt1Ty (*context) }, false),
		                      GlobalValue::ExternalLinkage, "f", module.get ());

		BasicBlock *entry = BasicBlock::Create (*context, "entry", f);
		BasicBlock *left_bb = BasicBlock::Create (*context, "left", f);
		BasicBlock *right_bb = BasicBlock::Create (*context, "right", f);
		BasicBlock *join = BasicBlock::Create (*context, "join", f);

		IRBuilder<> b (entry);
		b.CreateCondBr (f->getArg (0), left_bb, right_bb);

		b.SetInsertPoint (left_bb);
		left = b.CreateCall (make, {}, "left_value");
		b.CreateBr (join);

		b.SetInsertPoint (right_bb);
		right = b.CreateCall (make, {}, "right_value");
		b.CreateBr (join);

		b.SetInsertPoint (join);
		phi = b.CreatePHI (ptr, 2, "merged");
		phi->addIncoming (left, left_bb);
		phi->addIncoming (right, right_bb);
		b.CreateRet (phi);
	}
};

TEST (FoldCastTest, SettlesWhenEveryEdgeIsDecidedEvenIfTheyDisagree)
{
	MergeModule m;

	EXPECT_TRUE (isinst_settles_over_incoming (*m.phi, [&] (Value *v) {
		return v == m.left ? CastAnswer::Yes : CastAnswer::No;
	}));
}

TEST (FoldCastTest, RefusesWhenOneEdgeIsUndecided)
{
	MergeModule m;

	EXPECT_FALSE (isinst_settles_over_incoming (*m.phi, [&] (Value *v) {
		return v == m.left ? CastAnswer::Yes : CastAnswer::Unknown;
	}));
}

TEST (FoldCastTest, RebuildRefusesWithOneUndecidedEdge)
{
	MergeModule m;

	Value *rebuilt = rebuild_isinst_over_incoming (*m.phi, [&] (Value *v) {
		return v == m.left ? CastAnswer::Yes : CastAnswer::Unknown;
	});

	EXPECT_EQ (rebuilt, nullptr);
}

TEST (FoldCastTest, RebuildTakesTheOperandOnYesAndNullOnNo)
{
	MergeModule m;

	Value *rebuilt = rebuild_isinst_over_incoming (*m.phi, [&] (Value *v) {
		return v == m.left ? CastAnswer::Yes : CastAnswer::No;
	});

	ASSERT_NE (rebuilt, nullptr);
	EXPECT_FALSE (verifyFunction (*m.f, &errs ()));

	auto *rebuilt_phi = dyn_cast<PHINode> (rebuilt);

	ASSERT_NE (rebuilt_phi, nullptr);
	ASSERT_EQ (rebuilt_phi->getNumIncomingValues (), 2u);

	for (unsigned i = 0; i < 2; i++) {
		BasicBlock *from = rebuilt_phi->getIncomingBlock (i);
		Value *value = rebuilt_phi->getIncomingValue (i);

		if (from == m.phi->getIncomingBlock (0))
			EXPECT_EQ (value, m.left);
		else
			EXPECT_TRUE (isa<ConstantPointerNull> (value));
	}
}

} // namespace
} // namespace test
} // namespace mono
