/*
 * Tests for RgctxFetchPass, which reads a generic-context slot in front of the
 * call that fills it.
 *
 * Pure LLVM: the pass names no metadata, so neither do these. The walk the
 * translator would compute is written here by hand.
 */

#include "passes/rgctx-fetch.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
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

/// A module holding one fetch of a slot, either as a call or as an invoke with
/// a handler around it.
struct FetchModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *fill = nullptr;
	Function *caller = nullptr;
	CallBase *site = nullptr;

	/// The icall's signature spells the context and the slot as integers, which
	/// is what the runtime registered it with. as_pointer builds the same fetch
	/// with both of them as pointers.
	FetchModule (StringRef walk, bool protect = false, bool as_pointer = false)
	{
		module = std::make_unique<Module> ("rgctx", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *i32 = Type::getInt32Ty (*context);
		Type *word = as_pointer ? ptr : Type::getInt64Ty (*context);

		fill = Function::Create (FunctionType::get (word, { word, i32 }, false),
		                         GlobalValue::ExternalLinkage, "fill_rgctx",
		                         module.get ());
		fill->addFnAttr (rgctx_fetch_attribute);

		caller = Function::Create (FunctionType::get (word, { word }, false),
		                           GlobalValue::ExternalLinkage, "caller",
		                           module.get ());

		BasicBlock *entry = BasicBlock::Create (*context, "entry", caller);
		IRBuilder<> b (entry);
		Value *args[] = { caller->getArg (0), ConstantInt::get (i32, 2) };

		if (protect) {
			BasicBlock *returned = BasicBlock::Create (*context, "returned", caller);
			BasicBlock *pad = BasicBlock::Create (*context, "pad", caller);

			caller->setPersonalityFn (Function::Create (
				FunctionType::get (i32, true), GlobalValue::ExternalLinkage,
				"personality", module.get ()));
			site = b.CreateInvoke (fill, returned, pad, args);

			IRBuilder<> in_pad (pad);
			LandingPadInst *caught = in_pad.CreateLandingPad (
				StructType::get (ptr, i32), 0);

			caught->setCleanup (true);
			in_pad.CreateRet (Constant::getNullValue (word));
			b.SetInsertPoint (returned);
		} else {
			site = b.CreateCall (fill, args);
		}

		b.CreateRet (site);
		if (!walk.empty ())
			site->addFnAttr (Attribute::get (*context, rgctx_walk_attribute,
			                                 ("ctx=0,walk=" + walk).str ()));
	}

	void lower ()
	{
		ModuleAnalysisManager mam;

		RgctxFetchPass ().run (*module, mam);
	}

	unsigned count (unsigned opcode) const
	{
		unsigned found = 0;

		for (const Instruction &i : instructions (caller))
			if (i.getOpcode () == opcode)
				++found;

		return found;
	}

	/// The block that holds the fill call, or null when no call is left.
	const BasicBlock *fill_block () const
	{
		for (const User *user : fill->users ())
			if (const auto *call = dyn_cast<CallBase> (user))
				return call->getParent ();

		return nullptr;
	}
};

TEST (RgctxFetch, AOneStepWalkReadsTheSlotBeforeTheCall)
{
	FetchModule fetch ("24");

	fetch.lower ();
	EXPECT_FALSE (verifyFunction (*fetch.caller, &errs ()));
	EXPECT_EQ (fetch.count (Instruction::Load), 1u);
	EXPECT_EQ (fetch.count (Instruction::PHI), 1u);
	EXPECT_EQ (fetch.count (Instruction::Call), 1u);
}

TEST (RgctxFetch, EachOffsetInTheWalkIsALoadOfItsOwn)
{
	FetchModule fetch ("40:0:24");

	fetch.lower ();
	EXPECT_FALSE (verifyFunction (*fetch.caller, &errs ()));
	EXPECT_EQ (fetch.count (Instruction::Load), 3u);

	// One test for each load, and every one of them reaches the call.
	EXPECT_EQ (fetch.count (Instruction::ICmp), 3u);
	for (const BasicBlock &block : *fetch.caller)
		if (const auto *branch = dyn_cast<BranchInst> (block.getTerminator ()))
			if (branch->isConditional ())
				EXPECT_EQ (branch->getSuccessor (0), fetch.fill_block ());
}

TEST (RgctxFetch, TheValueTheSiteHandedOnComesFromEitherPath)
{
	FetchModule fetch ("24");

	fetch.lower ();

	const auto *returned = dyn_cast<ReturnInst> (
		fetch.caller->back ().getTerminator ());

	ASSERT_NE (returned, nullptr);

	const auto *slot = dyn_cast<PHINode> (returned->getReturnValue ());

	ASSERT_NE (slot, nullptr);
	EXPECT_EQ (slot->getNumIncomingValues (), 2u);
	EXPECT_TRUE (isa<LoadInst> (slot->getIncomingValue (0)));
	EXPECT_TRUE (isa<CallBase> (slot->getIncomingValue (1)));
}

TEST (RgctxFetch, AProtectedFetchKeepsItsHandler)
{
	FetchModule fetch ("24", /*protect=*/true);

	fetch.lower ();
	EXPECT_FALSE (verifyFunction (*fetch.caller, &errs ()));
	EXPECT_EQ (fetch.count (Instruction::Invoke), 1u);
	EXPECT_EQ (fetch.count (Instruction::PHI), 1u);

	const auto *invoke = dyn_cast<InvokeInst> (fetch.fill_block ()->getTerminator ());

	ASSERT_NE (invoke, nullptr);
	EXPECT_EQ (invoke->getUnwindDest ()->getName (), "pad");
}

TEST (RgctxFetch, AContextThatArrivesAsAPointerIsWalkedAsOne)
{
	FetchModule fetch ("40:24", /*protect=*/false, /*as_pointer=*/true);

	fetch.lower ();
	EXPECT_FALSE (verifyFunction (*fetch.caller, &errs ()));
	EXPECT_EQ (fetch.count (Instruction::IntToPtr), 0u);
	EXPECT_EQ (fetch.count (Instruction::Load), 2u);
}

TEST (RgctxFetch, ASiteWithNoWalkKeepsItsCall)
{
	FetchModule fetch ("");

	fetch.lower ();
	EXPECT_EQ (fetch.count (Instruction::Load), 0u);
	EXPECT_EQ (fetch.count (Instruction::PHI), 0u);
	EXPECT_EQ (fetch.count (Instruction::Call), 1u);
}

} // namespace
} // namespace test
} // namespace mono
