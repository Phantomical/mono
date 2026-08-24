/*
 * Tests for RgctxDedupPass, which drops a generic-context fetch that a
 * dominating fetch already did.
 *
 * Pure LLVM: the pass names no metadata, so neither do these. The walk the
 * translator would compute is written here by hand, one for each slot.
 */

#include "passes/rgctx-dedup.hpp"
#include "passes/rgctx-fetch.hpp"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// A module with one caller and the icall that fills a slot, ready for fetches
/// the case adds itself.
struct DedupModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *fill = nullptr;
	Function *caller = nullptr;
	Type *word = nullptr;
	Type *i32 = nullptr;

	/// The caller takes two contexts, so a case can fetch one slot from each.
	DedupModule ()
	{
		module = std::make_unique<Module> ("rgctx", *context);

		word = Type::getInt64Ty (*context);
		i32 = Type::getInt32Ty (*context);

		fill = Function::Create (FunctionType::get (word, { word, i32 }, false),
		                         GlobalValue::ExternalLinkage, "fill_rgctx",
		                         module.get ());
		fill->addFnAttr (rgctx_fetch_attribute);

		caller = Function::Create (FunctionType::get (word, { word, word }, false),
		                           GlobalValue::ExternalLinkage, "caller",
		                           module.get ());
		caller->setPersonalityFn (Function::Create (
			FunctionType::get (i32, true), GlobalValue::ExternalLinkage,
			"personality", module.get ()));
	}

	BasicBlock *block (StringRef name)
	{
		return BasicBlock::Create (*context, name, caller);
	}

	/// A landing pad that returns, for a fetch a clause protects.
	BasicBlock *pad ()
	{
		BasicBlock *caught = block ("pad");
		IRBuilder<> b (caught);
		LandingPadInst *landed = b.CreateLandingPad (
			StructType::get (PointerType::get (*context, 0), i32), 0);

		landed->setCleanup (true);

		return caught;
	}

	/// Writes a fetch of the slot at index, out of the context in ctx. With a
	/// handler it becomes an invoke, and the builder continues on the normal
	/// edge.
	CallBase *fetch (IRBuilder<> &b, Value *ctx, unsigned index,
	                 BasicBlock *handler = nullptr)
	{
		Value *args[] = { ctx, ConstantInt::get (i32, index) };
		CallBase *site = nullptr;

		if (handler != nullptr) {
			BasicBlock *returned = block ("returned");

			site = b.CreateInvoke (fill, returned, handler, args);
			b.SetInsertPoint (returned);
		} else {
			site = b.CreateCall (fill, args);
		}

		// What walk_to_slot () computes: one offset for each slot.
		site->addFnAttr (Attribute::get (
			*context, rgctx_walk_attribute,
			"ctx=0,walk=" + std::to_string (24 + 8 * index)));

		return site;
	}

	void dedup ()
	{
		PassBuilder pb;
		FunctionAnalysisManager fam;

		pb.registerFunctionAnalyses (fam);
		RgctxDedupPass ().run (*caller, fam);
	}

	unsigned fetches () const
	{
		unsigned found = 0;

		for (const Instruction &i : instructions (caller))
			if (const auto *site = dyn_cast<CallBase> (&i))
				if (site->getCalledFunction () == fill)
					++found;

		return found;
	}
};

TEST (RgctxDedup, ADominatedFetchTakesTheValueOfTheOneAboveIt)
{
	DedupModule m;
	IRBuilder<> b (m.block ("entry"));
	CallBase *first = m.fetch (b, m.caller->getArg (0), 2);

	m.fetch (b, m.caller->getArg (0), 2);
	b.CreateRet (first);

	m.dedup ();
	EXPECT_FALSE (verifyFunction (*m.caller, &errs ()));
	EXPECT_EQ (m.fetches (), 1u);
}

TEST (RgctxDedup, TheUsesOfTheDroppedFetchMoveToTheOneThatStays)
{
	DedupModule m;
	IRBuilder<> b (m.block ("entry"));
	CallBase *first = m.fetch (b, m.caller->getArg (0), 2);
	CallBase *second = m.fetch (b, m.caller->getArg (0), 2);

	b.CreateRet (second);

	m.dedup ();

	const auto *returned = dyn_cast<ReturnInst> (m.caller->back ().getTerminator ());

	ASSERT_NE (returned, nullptr);
	EXPECT_EQ (returned->getReturnValue (), first);
}

TEST (RgctxDedup, TwoSlotsOfOneContextAreBothKept)
{
	DedupModule m;
	IRBuilder<> b (m.block ("entry"));
	CallBase *first = m.fetch (b, m.caller->getArg (0), 2);

	m.fetch (b, m.caller->getArg (0), 3);
	b.CreateRet (first);

	m.dedup ();
	EXPECT_EQ (m.fetches (), 2u);
}

TEST (RgctxDedup, OneSlotOfTwoContextsIsFetchedFromEach)
{
	DedupModule m;
	IRBuilder<> b (m.block ("entry"));
	CallBase *first = m.fetch (b, m.caller->getArg (0), 2);

	m.fetch (b, m.caller->getArg (1), 2);
	b.CreateRet (first);

	m.dedup ();
	EXPECT_EQ (m.fetches (), 2u);
}

TEST (RgctxDedup, AFetchTheHandlerReachesKeepsItsCall)
{
	DedupModule m;
	IRBuilder<> b (m.block ("entry"));
	BasicBlock *caught = m.pad ();
	BasicBlock *join = m.block ("join");

	m.fetch (b, m.caller->getArg (0), 2, caught);
	b.CreateBr (join);
	IRBuilder<> (caught).CreateBr (join);

	b.SetInsertPoint (join);
	b.CreateRet (m.fetch (b, m.caller->getArg (0), 2));

	m.dedup ();
	EXPECT_FALSE (verifyFunction (*m.caller, &errs ()));

	// The protected fetch reaches the second one along its unwind edge as
	// well, and the slot is still empty there.
	EXPECT_EQ (m.fetches (), 2u);
}

TEST (RgctxDedup, ADroppedInvokeLeavesAnUnconditionalBranch)
{
	DedupModule m;
	IRBuilder<> b (m.block ("entry"));
	CallBase *first = m.fetch (b, m.caller->getArg (0), 2);
	BasicBlock *caught = m.pad ();

	IRBuilder<> (caught).CreateRet (ConstantInt::get (m.word, 0));
	m.fetch (b, m.caller->getArg (0), 2, caught);
	b.CreateRet (first);

	m.dedup ();
	EXPECT_FALSE (verifyFunction (*m.caller, &errs ()));
	EXPECT_EQ (m.fetches (), 1u);
	EXPECT_TRUE (pred_empty (caught));
}

TEST (RgctxDedup, AChainOfFetchesAllTakesTheOneAtTheTop)
{
	DedupModule m;
	IRBuilder<> b (m.block ("entry"));
	CallBase *first = m.fetch (b, m.caller->getArg (0), 2);
	BasicBlock *middle = m.block ("middle");
	BasicBlock *last = m.block ("last");

	b.CreateBr (middle);
	b.SetInsertPoint (middle);
	m.fetch (b, m.caller->getArg (0), 2);
	b.CreateBr (last);
	b.SetInsertPoint (last);

	CallBase *third = m.fetch (b, m.caller->getArg (0), 2);

	b.CreateRet (third);

	m.dedup ();
	EXPECT_FALSE (verifyFunction (*m.caller, &errs ()));
	EXPECT_EQ (m.fetches (), 1u);

	const auto *returned = dyn_cast<ReturnInst> (last->getTerminator ());

	ASSERT_NE (returned, nullptr);
	EXPECT_EQ (returned->getReturnValue (), first);
}

TEST (RgctxDedup, AFetchWithNoWalkKeepsItsCall)
{
	DedupModule m;
	IRBuilder<> b (m.block ("entry"));
	Value *args[] = { m.caller->getArg (0), ConstantInt::get (m.i32, 2) };
	CallBase *first = b.CreateCall (m.fill, args);

	b.CreateCall (m.fill, args);
	b.CreateRet (first);

	m.dedup ();
	EXPECT_EQ (m.fetches (), 2u);
}

} // namespace
} // namespace test
} // namespace mono
