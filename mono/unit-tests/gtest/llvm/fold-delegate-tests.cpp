/*
 * Tests for delegate_target_at (), which reads what the IR says about the
 * delegate arriving at a site.
 *
 * Pure LLVM. The walk compares MonoMethod pointers and never dereferences one,
 * so a test marks values with addresses of its own and needs no runtime to
 * build or to read.
 */

#include "passes/fold-delegate.hpp"

#include "analysis/constant-values.hpp"

#include "analysis/operand-class.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// Two methods a mark can name. The walk only ever compares them, so what they
/// point at is never read.
MonoMethod *const first = reinterpret_cast<MonoMethod *> (0x1000);
MonoMethod *const second = reinterpret_cast<MonoMethod *> (0x2000);

/// A function whose entry branches to a merge, so a test can hand each arm a
/// value of its own.
struct MergeModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	BasicBlock *entry = nullptr;
	BasicBlock *left = nullptr;
	BasicBlock *right = nullptr;
	BasicBlock *merge = nullptr;

	MergeModule ()
	{
		module = std::make_unique<Module> ("merge", *context);

		Type *ptr = PointerType::get (*context, 0);

		caller = Function::Create (
			FunctionType::get (ptr, { Type::getInt1Ty (*context), ptr }, false),
			GlobalValue::ExternalLinkage, "caller", module.get ());

		entry = BasicBlock::Create (*context, "entry", caller);
		left = BasicBlock::Create (*context, "left", caller);
		right = BasicBlock::Create (*context, "right", caller);
		merge = BasicBlock::Create (*context, "merge", caller);

		IRBuilder<> (entry).CreateCondBr (caller->getArg (0), left, right);
		IRBuilder<> (left).CreateBr (merge);
		IRBuilder<> (right).CreateBr (merge);
	}

	/// An instruction in \p block that produces a pointer, marked with \p target
	/// when one is given.
	///
	/// A load rather than anything cheaper, because the builder folds an
	/// operation over constants into a constant expression and the mark has to
	/// sit on an instruction.
	Instruction *produce (BasicBlock *block, MonoMethod *target)
	{
		IRBuilder<> b (block->getTerminator ());
		Instruction *made = b.CreateLoad (b.getPtrTy (), caller->getArg (1));

		if (target != nullptr)
			mark_delegate_target (*made, target);

		return made;
	}

	/// A phi in the merge block over the two arms.
	PHINode *joined (Value *from_left, Value *from_right)
	{
		IRBuilder<> b (merge);
		PHINode *phi = b.CreatePHI (b.getPtrTy (), 2);

		phi->addIncoming (from_left, left);
		phi->addIncoming (from_right, right);
		b.CreateRet (phi);

		return phi;
	}
};

/// Settles \p f, which the queries below take.
ConstantValues
settled_for (const Function &f)
{
	FunctionAnalysisManager fam;
	ModuleAnalysisManager mam;
	LoopAnalysisManager lam;
	CGSCCAnalysisManager cgam;
	PassBuilder pb;

	// All four, because MemorySSA asks alias analysis for GlobalsAA, which is
	// a module analysis it reaches through the proxy.
	pb.registerModuleAnalyses (mam);
	pb.registerCGSCCAnalyses (cgam);
	pb.registerFunctionAnalyses (fam);
	pb.registerLoopAnalyses (lam);
	pb.crossRegisterProxies (lam, fam, cgam, mam);

	return MonoConstantValues ().run (const_cast<Function &> (f), fam);
}

TEST (FoldDelegateTest, ReadsAMarkedProducer)
{
	MergeModule m;
	DelegateTarget found = delegate_target_at (m.produce (m.left, first),
	                                                  settled_for (*m.caller));

	EXPECT_EQ (found.method, first);
	EXPECT_TRUE (found.settled);
}

TEST (FoldDelegateTest, SaysNothingAboutAnUnmarkedValue)
{
	MergeModule m;
	DelegateTarget found = delegate_target_at (m.caller->getArg (1),
	                                                  settled_for (*m.caller));

	EXPECT_EQ (found.method, nullptr);
	EXPECT_FALSE (found.settled);
}

TEST (FoldDelegateTest, SettlesAMergeWhoseArmsAgree)
{
	MergeModule m;
	DelegateTarget found = delegate_target_at (
		m.joined (m.produce (m.left, first), m.produce (m.right, first)),
	                                                  settled_for (*m.caller));

	EXPECT_EQ (found.method, first);
	EXPECT_TRUE (found.settled);
}

TEST (FoldDelegateTest, OffersACandidateWhenOneArmIsOpaque)
{
	MergeModule m;
	DelegateTarget found = delegate_target_at (
		m.joined (m.produce (m.left, first), m.produce (m.right, nullptr)),
	                                                  settled_for (*m.caller));

	EXPECT_EQ (found.method, first);
	EXPECT_FALSE (found.settled);
}

TEST (FoldDelegateTest, RefusesAMergeWhoseArmsDisagree)
{
	MergeModule m;
	DelegateTarget found = delegate_target_at (
		m.joined (m.produce (m.left, first), m.produce (m.right, second)),
	                                                  settled_for (*m.caller));

	// Naming one of them would be a guess, and a profile is what would have to
	// settle it.
	EXPECT_EQ (found.method, nullptr);
	EXPECT_FALSE (found.settled);
}

TEST (FoldDelegateTest, ReadsThroughASelect)
{
	MergeModule m;
	IRBuilder<> b (m.merge);
	Value *pick = b.CreateSelect (m.caller->getArg (0),
	                              m.produce (m.left, first),
	                              m.produce (m.right, first));

	b.CreateRet (pick);

	DelegateTarget found = delegate_target_at (pick,
	                                                  settled_for (*m.caller));

	EXPECT_EQ (found.method, first);
	EXPECT_TRUE (found.settled);
}

TEST (FoldDelegateTest, ReadsThroughAFreeze)
{
	MergeModule m;
	IRBuilder<> b (m.merge);

	// SimpleLoopUnswitch freezes a condition it hoists out of a loop, and a null
	// test on the delegate is such a condition. A real body reaches the walk in
	// this shape.
	Value *frozen = b.CreateFreeze (m.produce (m.left, first));

	b.CreateRet (frozen);

	DelegateTarget found = delegate_target_at (frozen);

	EXPECT_EQ (found.method, first);
	EXPECT_TRUE (found.settled);
}

TEST (FoldDelegateTest, TerminatesOnAMergeThatReachesItself)
{
	MergeModule m;
	IRBuilder<> b (m.merge);
	PHINode *phi = b.CreatePHI (b.getPtrTy (), 2);

	phi->addIncoming (m.produce (m.left, first), m.left);
	phi->addIncoming (phi, m.right);
	b.CreateRet (phi);

	// The value going round the loop is the phi, so it says nothing the arm
	// below it has not already said.
	DelegateTarget found = delegate_target_at (phi,
	                                                  settled_for (*m.caller));

	EXPECT_EQ (found.method, first);
	EXPECT_TRUE (found.settled);
}

} // namespace
} // namespace test
} // namespace mono
