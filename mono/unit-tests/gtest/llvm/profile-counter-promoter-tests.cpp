/*
 * Tests for ProfileCounterPromoterPass, which takes a counter update out of the
 * loop it sits in and writes the count back at each exit.
 *
 * Pure LLVM: the pass names no metadata, so neither do these.
 */

#include "passes/profile-counter-promoter.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// A module holding one function whose loops carry instrprof increments.
///
/// Each loop is `for (i = 0; i < n; i++)`, built in the shape LoopSimplify
/// leaves: a preheader of its own, and exit blocks nothing outside it branches
/// to. A case that wants a loop without one of those breaks it by hand.
struct LoopCounterModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Type *i1 = nullptr;
	Type *i32 = nullptr;
	Type *i64 = nullptr;
	GlobalVariable *profile_name = nullptr;
	Function *body = nullptr;
	/// The block a case builds into next.
	BasicBlock *tail = nullptr;

	static constexpr uint64_t function_hash = 0x1234;
	static constexpr unsigned counter_slots = 8;

	/// One loop's blocks, named the way LoopInfo talks about them.
	struct Loop {
		BasicBlock *preheader = nullptr;
		BasicBlock *header = nullptr;
		BasicBlock *latch = nullptr;
		BasicBlock *exit = nullptr;
		PHINode *iv = nullptr;
	};

	LoopCounterModule ()
	{
		module = std::make_unique<Module> ("counters", *context);
		i1 = Type::getInt1Ty (*context);
		i32 = Type::getInt32Ty (*context);
		i64 = Type::getInt64Ty (*context);

		Constant *text = ConstantDataArray::getString (*context, "body");

		profile_name = new GlobalVariable (*module, text->getType (), true,
		                                   GlobalValue::PrivateLinkage, text,
		                                   "__profn_body");

		FunctionType *type =
			FunctionType::get (Type::getVoidTy (*context), { i32, i1 }, false);

		body = Function::Create (type, GlobalValue::ExternalLinkage, "body",
		                         module.get ());
		tail = BasicBlock::Create (*context, "entry", body);
	}

	/// The loop trip count, and the opaque condition an early exit is taken on.
	Value *trips () const { return body->getArg (0); }
	Value *flag () const { return body->getArg (1); }

	/// Builds a loop that \p from runs into, keeping wherever \p from went.
	Loop add_loop (BasicBlock *from, StringRef tag)
	{
		BasicBlock *after = nullptr;

		if (auto *br = dyn_cast_or_null<BranchInst> (from->getTerminator ())) {
			after = br->getSuccessor (0);
			br->eraseFromParent ();
		}

		Loop l;

		l.preheader = BasicBlock::Create (*context, tag + ".preheader", body);
		l.header = BasicBlock::Create (*context, tag + ".header", body);
		l.latch = BasicBlock::Create (*context, tag + ".latch", body);
		l.exit = BasicBlock::Create (*context, tag + ".exit", body);

		IRBuilder<> (from).CreateBr (l.preheader);
		IRBuilder<> (l.preheader).CreateBr (l.header);

		IRBuilder<> in_header (l.header);

		l.iv = in_header.CreatePHI (i32, 2, tag + ".iv");
		l.iv->addIncoming (ConstantInt::get (i32, 0), l.preheader);
		in_header.CreateBr (l.latch);

		IRBuilder<> in_latch (l.latch);
		Value *next = in_latch.CreateAdd (l.iv, ConstantInt::get (i32, 1));

		l.iv->addIncoming (next, l.latch);
		in_latch.CreateCondBr (in_latch.CreateICmpSLT (next, trips ()), l.header,
		                       l.exit);

		if (after != nullptr)
			IRBuilder<> (l.exit).CreateBr (after);
		else
			tail = l.exit;

		return l;
	}

	/// A second way out of \p l, taken from its header on the opaque condition.
	///
	/// The loop comes out with two exit blocks and two exiting blocks. The
	/// block this adds is an exit of its own rather than another edge into the
	/// loop's normal exit, which would put a predecessor outside the loop on
	/// that exit and cost the loop its dedicated exits.
	BasicBlock *add_early_exit (Loop &l, StringRef tag)
	{
		BasicBlock *early = BasicBlock::Create (*context, tag, body);
		BranchInst *to_latch = cast<BranchInst> (l.header->getTerminator ());

		IRBuilder<> (to_latch).CreateCondBr (flag (), early, l.latch);
		to_latch->eraseFromParent ();

		if (auto *br = dyn_cast_or_null<BranchInst> (l.exit->getTerminator ())) {
			IRBuilder<> (early).CreateBr (br->getSuccessor (0));
		} else {
			BasicBlock *join = BasicBlock::Create (*context, tag + ".join", body);

			IRBuilder<> (l.exit).CreateBr (join);
			IRBuilder<> (early).CreateBr (join);
			tail = join;
		}

		return early;
	}

	/// Puts a block inside \p l that only the opaque condition reaches.
	BasicBlock *split_header_on_a_condition (Loop &l, StringRef tag)
	{
		BasicBlock *taken = BasicBlock::Create (*context, tag, body, l.latch);
		BranchInst *to_latch = cast<BranchInst> (l.header->getTerminator ());

		IRBuilder<> (to_latch).CreateCondBr (flag (), taken, l.latch);
		to_latch->eraseFromParent ();
		IRBuilder<> (taken).CreateBr (l.latch);

		return taken;
	}

	/// Reaches the header of \p l from outside it a second time, which is what
	/// leaves the loop without a preheader.
	void give_the_header_a_second_entry (Loop &l)
	{
		BasicBlock &entry = body->getEntryBlock ();
		BranchInst *to_preheader = cast<BranchInst> (entry.getTerminator ());

		IRBuilder<> (to_preheader).CreateCondBr (flag (), l.preheader, l.header);
		to_preheader->eraseFromParent ();
		l.iv->addIncoming (ConstantInt::get (i32, 0), &entry);
	}

	/// Reaches the exit of \p l from outside the loop, which is what leaves it
	/// without dedicated exits.
	void branch_to_the_exit_from_outside (Loop &l)
	{
		BasicBlock &entry = body->getEntryBlock ();
		BranchInst *to_preheader = cast<BranchInst> (entry.getTerminator ());

		IRBuilder<> (to_preheader).CreateCondBr (flag (), l.preheader, l.exit);
		to_preheader->eraseFromParent ();
	}

	/// `llvm.instrprof.increment (@__profn_body, hash, slots, index)`.
	void increment (BasicBlock *bb, unsigned index)
	{
		IRBuilder<> b (bb);

		if (bb->getTerminator () != nullptr)
			b.SetInsertPoint (bb->getTerminator ());

		b.CreateIntrinsic (Intrinsic::instrprof_increment,
		                   { profile_name, ConstantInt::get (i64, function_hash),
		                     ConstantInt::get (i32, counter_slots),
		                     ConstantInt::get (i32, index) });
	}

	/// Closes the function and runs the pass over it.
	PreservedAnalyses promote (PromotionPolicy policy = PromotionPolicy ())
	{
		if (tail != nullptr && tail->getTerminator () == nullptr)
			IRBuilder<> (tail).CreateRetVoid ();

		ModuleAnalysisManager mam;
		FunctionAnalysisManager fam;
		LoopAnalysisManager lam;
		CGSCCAnalysisManager cgam;
		PassBuilder pb;

		pb.registerModuleAnalyses (mam);
		pb.registerFunctionAnalyses (fam);
		pb.registerLoopAnalyses (lam);
		pb.registerCGSCCAnalyses (cgam);
		pb.crossRegisterProxies (lam, fam, cgam, mam);

		return ProfileCounterPromoterPass (policy).run (*module, mam);
	}

	/// Returns the increments \p bb still holds that carry no step of their own.
	std::vector<InstrProfIncrementInst *> plain_in (const BasicBlock *bb) const
	{
		std::vector<InstrProfIncrementInst *> found;

		for (const Instruction &i : *bb)
			if (auto *inc =
			            dyn_cast<InstrProfIncrementInst> (const_cast<Instruction *> (&i)))
				if (!isa<InstrProfIncrementInstStep> (inc))
					found.push_back (inc);

		return found;
	}

	std::vector<InstrProfIncrementInstStep *> steps_in (const BasicBlock *bb) const
	{
		std::vector<InstrProfIncrementInstStep *> found;

		for (const Instruction &i : *bb)
			if (auto *step = dyn_cast<InstrProfIncrementInstStep> (
				    const_cast<Instruction *> (&i)))
				found.push_back (step);

		return found;
	}

	/// Returns every write-back the function holds, wherever it landed.
	std::vector<InstrProfIncrementInstStep *> all_steps () const
	{
		std::vector<InstrProfIncrementInstStep *> found;

		for (Instruction &i : instructions (*body))
			if (auto *step = dyn_cast<InstrProfIncrementInstStep> (&i))
				found.push_back (step);

		return found;
	}

	unsigned phis_of (Type *type, const BasicBlock *bb) const
	{
		unsigned n = 0;

		for (const PHINode &phi : bb->phis ())
			if (phi.getType () == type)
				n++;

		return n;
	}
};

} // namespace

TEST (CounterPromotion, AnIncrementLeavesTheLoopForItsExit)
{
	LoopCounterModule m;
	LoopCounterModule::Loop l = m.add_loop (&m.body->getEntryBlock (), "l");

	m.increment (l.header, 3);
	m.promote ();

	EXPECT_TRUE (m.plain_in (l.header).empty ());

	std::vector<InstrProfIncrementInstStep *> steps = m.steps_in (l.exit);

	ASSERT_EQ (steps.size (), 1u);
	EXPECT_EQ (steps[0]->getIndex ()->getZExtValue (), 3u);
	EXPECT_EQ (steps[0]->getNumCounters ()->getZExtValue (),
	           LoopCounterModule::counter_slots);
	// The step is the accumulator, so it is an SSA value rather than the
	// constant 1 the plain increment carried.
	EXPECT_FALSE (isa<Constant> (steps[0]->getStep ()));
	EXPECT_EQ (m.phis_of (m.i64, l.header), 1u);
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

TEST (CounterPromotion, TwoCountersInOneLoopGetAnAccumulatorEach)
{
	LoopCounterModule m;
	LoopCounterModule::Loop l = m.add_loop (&m.body->getEntryBlock (), "l");

	m.increment (l.header, 1);
	m.increment (l.header, 2);
	m.promote ();

	std::vector<InstrProfIncrementInstStep *> steps = m.steps_in (l.exit);

	ASSERT_EQ (steps.size (), 2u);
	EXPECT_EQ (steps[0]->getIndex ()->getZExtValue (), 1u);
	EXPECT_EQ (steps[1]->getIndex ()->getZExtValue (), 2u);
	EXPECT_NE (steps[0]->getStep (), steps[1]->getStep ());
	EXPECT_EQ (m.phis_of (m.i64, l.header), 2u);
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

/*
 * A counter on one arm of a branch inside the loop. The accumulator has to
 * carry the count of the path taken rather than the count of turns.
 */
TEST (CounterPromotion, AConditionalIncrementAccumulatesOnItsOwnPath)
{
	LoopCounterModule m;
	LoopCounterModule::Loop l = m.add_loop (&m.body->getEntryBlock (), "l");
	BasicBlock *taken = m.split_header_on_a_condition (l, "taken");

	m.increment (taken, 0);
	m.promote ();

	EXPECT_TRUE (m.plain_in (taken).empty ());
	ASSERT_EQ (m.steps_in (l.exit).size (), 1u);
	// One where the two arms meet, and one carrying the count around the loop.
	EXPECT_EQ (m.phis_of (m.i64, l.latch), 1u);
	EXPECT_EQ (m.phis_of (m.i64, l.header), 1u);
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

/*
 * Promoting the inner loop leaves a write-back in each of its two exits, both
 * inside the outer loop. LLVM's own promoter takes each of those out on its own
 * and writes the counter back twice for each way out of the outer loop.
 */
TEST (CounterPromotion, AnInnerLoopsTwoWritebacksMergeIntoOneAccumulator)
{
	LoopCounterModule m;
	LoopCounterModule::Loop outer = m.add_loop (&m.body->getEntryBlock (), "outer");
	LoopCounterModule::Loop inner = m.add_loop (outer.header, "inner");

	m.add_early_exit (inner, "inner.early");
	m.increment (inner.header, 5);
	m.promote ();

	EXPECT_TRUE (m.plain_in (inner.header).empty ());

	std::vector<InstrProfIncrementInstStep *> steps = m.all_steps ();

	ASSERT_EQ (steps.size (), 1u);
	EXPECT_EQ (steps[0]->getParent (), outer.exit);
	EXPECT_EQ (steps[0]->getIndex ()->getZExtValue (), 5u);
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

TEST (CounterPromotion, ALoopWithNoPreheaderIsLeftAlone)
{
	LoopCounterModule m;
	LoopCounterModule::Loop l = m.add_loop (&m.body->getEntryBlock (), "l");

	m.increment (l.header, 0);
	m.give_the_header_a_second_entry (l);
	m.promote ();

	EXPECT_EQ (m.plain_in (l.header).size (), 1u);
	EXPECT_TRUE (m.all_steps ().empty ());
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

TEST (CounterPromotion, ALoopWhoseExitIsReachedFromOutsideIsLeftAlone)
{
	LoopCounterModule m;
	LoopCounterModule::Loop l = m.add_loop (&m.body->getEntryBlock (), "l");

	m.increment (l.header, 0);
	m.branch_to_the_exit_from_outside (l);
	m.promote ();

	EXPECT_EQ (m.plain_in (l.header).size (), 1u);
	EXPECT_TRUE (m.all_steps ().empty ());
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

TEST (CounterPromotion, TooManyExitingBlocksRefusesTheWholeLoop)
{
	LoopCounterModule m;
	LoopCounterModule::Loop l = m.add_loop (&m.body->getEntryBlock (), "l");
	PromotionPolicy policy;

	// The header now leaves the loop as well as the latch.
	m.add_early_exit (l, "early");
	m.increment (l.header, 0);
	policy.max_exiting = 1;
	m.promote (policy);

	EXPECT_EQ (m.plain_in (l.header).size (), 1u);
	EXPECT_TRUE (m.all_steps ().empty ());
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

/*
 * The default is off, because almost every loop a C# method ends with can be
 * left through a return. The setting still has to work: turning it on is what
 * a profile read while the code runs, rather than at exit, wants.
 */
TEST (CounterPromotion, SkipRetExitBlockRefusesALoopThatReturns)
{
	LoopCounterModule m;
	LoopCounterModule::Loop l = m.add_loop (&m.body->getEntryBlock (), "l");
	PromotionPolicy policy;

	m.increment (l.header, 0);
	policy.skip_ret_exit_block = true;
	m.promote (policy);

	EXPECT_TRUE (isa<ReturnInst> (l.exit->getTerminator ()));
	EXPECT_EQ (m.plain_in (l.header).size (), 1u);
	EXPECT_TRUE (m.all_steps ().empty ());
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

TEST (CounterPromotion, TheSameLoopPromotesUnderTheDefaultPolicy)
{
	LoopCounterModule m;
	LoopCounterModule::Loop l = m.add_loop (&m.body->getEntryBlock (), "l");

	m.increment (l.header, 0);
	m.promote ();

	EXPECT_TRUE (isa<ReturnInst> (l.exit->getTerminator ()));
	EXPECT_TRUE (m.plain_in (l.header).empty ());
	EXPECT_EQ (m.steps_in (l.exit).size (), 1u);
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

TEST (CounterPromotion, AnIncrementOutsideAnyLoopIsLeftAlone)
{
	LoopCounterModule m;

	m.increment (&m.body->getEntryBlock (), 0);
	m.promote ();

	EXPECT_EQ (m.plain_in (&m.body->getEntryBlock ()).size (), 1u);
	EXPECT_TRUE (m.all_steps ().empty ());
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

TEST (CounterPromotion, AFunctionWithNoIncrementsKeepsItsAnalyses)
{
	LoopCounterModule m;

	m.add_loop (&m.body->getEntryBlock (), "l");

	EXPECT_TRUE (m.promote ().areAllPreserved ());
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

TEST (CounterPromotion, TheEnabledFlagTurnsThePassOff)
{
	LoopCounterModule m;
	LoopCounterModule::Loop l = m.add_loop (&m.body->getEntryBlock (), "l");
	PromotionPolicy policy;

	m.increment (l.header, 0);
	policy.enabled = false;

	EXPECT_TRUE (m.promote (policy).areAllPreserved ());
	EXPECT_EQ (m.plain_in (l.header).size (), 1u);
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

} // namespace test
} // namespace mono
