/*
 * Tests for ClampFrameAlignPass, which takes a stack object's alignment down to
 * what a frame that is never realigned gives.
 *
 * Pure LLVM: the pass reads one function attribute and the data layout, so
 * neither of these builds a method.
 */

#include "passes/clamp-frame-align.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
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

/// The stack alignment amd64 gives, which is the number the pass lowers to.
constexpr uint64_t stack_align = 16;

/// The alignment a vectorizer asks for and this frame cannot hold.
constexpr uint64_t over_align = 32;

/// One function with an over-aligned frame object, a store into a slice of it,
/// a memset over it, and a store through a pointer the frame does not own.
struct FrameModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *f = nullptr;
	AllocaInst *slot = nullptr;
	StoreInst *into_slot = nullptr;
	StoreInst *into_argument = nullptr;
	MemSetInst *clear = nullptr;

	explicit FrameModule (bool has_stack_alignment = true)
	{
		module = std::make_unique<Module> ("frame", *context);
		module->setDataLayout (has_stack_alignment
			                       ? "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
			                       : "e-m:e-i64:64-f80:128-n8:16:32:64");

		Type *ptr = PointerType::get (*context, 0);
		Type *word = Type::getInt64Ty (*context);

		f = Function::Create (FunctionType::get (Type::getVoidTy (*context),
		                                        { ptr }, false),
		                      GlobalValue::ExternalLinkage, "body", module.get ());

		IRBuilder<> b (BasicBlock::Create (*context, "entry", f));

		slot = b.CreateAlloca (ArrayType::get (word, 6));
		slot->setAlignment (Align (over_align));

		clear = cast<MemSetInst> (b.CreateMemSet (slot, b.getInt8 (0),
		                                          b.getInt64 (48),
		                                          Align (over_align)));

		// Through a gep, which is how a slice of the object is reached and
		// what getUnderlyingObject () has to walk back.
		Value *slice = b.CreateConstInBoundsGEP1_64 (word, slot, 2);

		into_slot = b.CreateAlignedStore (b.getInt64 (1), slice,
		                                  Align (over_align));
		into_argument = b.CreateAlignedStore (b.getInt64 (1), f->getArg (0),
		                                      Align (over_align));
		b.CreateRetVoid ();
	}

	/// The pass asks for no analysis, so an empty manager serves it.
	void run ()
	{
		FunctionAnalysisManager fam;

		ClampFrameAlignPass ().run (*f, fam);
		ASSERT_FALSE (verifyFunction (*f, &errs ()));
	}
};

TEST (ClampFrameAlign, LowersTheSlotAndWhatReachesIt)
{
	FrameModule m;

	m.f->addFnAttr ("no-realign-stack");
	m.run ();

	EXPECT_EQ (m.slot->getAlign ().value (), stack_align);
	EXPECT_EQ (m.into_slot->getAlign ().value (), stack_align);
	ASSERT_TRUE (m.clear->getDestAlign ().has_value ());
	EXPECT_EQ (m.clear->getDestAlign ()->value (), stack_align);
}

TEST (ClampFrameAlign, LeavesAStoreThroughAnArgumentAlone)
{
	FrameModule m;

	m.f->addFnAttr ("no-realign-stack");
	m.run ();

	EXPECT_EQ (m.into_argument->getAlign ().value (), over_align);
}

TEST (ClampFrameAlign, LeavesAFrameThatCanRealignAlone)
{
	FrameModule m;

	m.run ();

	EXPECT_EQ (m.slot->getAlign ().value (), over_align);
	EXPECT_EQ (m.into_slot->getAlign ().value (), over_align);
}

TEST (ClampFrameAlign, LeavesADataLayoutWithNoStackAlignmentAlone)
{
	FrameModule m (/*has_stack_alignment=*/false);

	m.f->addFnAttr ("no-realign-stack");
	m.run ();

	EXPECT_EQ (m.slot->getAlign ().value (), over_align);
	EXPECT_EQ (m.into_slot->getAlign ().value (), over_align);
}

} // namespace
} // namespace test
} // namespace mono
