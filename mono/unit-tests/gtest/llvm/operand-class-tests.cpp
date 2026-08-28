/*
 * Tests for operand_class () and exact_class (), the walk that answers what
 * class a value is, including where the value is a merge.
 *
 * Most cases are pure LLVM. Every producer the walk reads is marked with a
 * fake MonoClass pointer of a test's own choosing, and no test asks a runtime
 * for anything.
 *
 * The allocation-site cases near the end are the exception. Reading a class
 * off an allocation's vtable operand checks that class is not marshal-by-ref
 * or a COM object, and that check reads real fields, so those cases boot a
 * runtime and mark a real class.
 */

#include "operand-class.hpp"

#include "harness.hpp"
#include "method-symbols.hpp"
#include "passes/alloc-func.hpp"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/class.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <gtest/gtest.h>

#include <memory>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/// Two classes a mark can name. The walk only ever compares them, so what
/// they point at is never read.
MonoClass *const classX = reinterpret_cast<MonoClass *> (0x1000);
MonoClass *const classY = reinterpret_cast<MonoClass *> (0x2000);

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

	/// A load in \p block, marked as an instance of \p klass when \p klass is
	/// given.
	///
	/// A load rather than anything cheaper, because the builder folds an
	/// operation over constants into a constant expression and a mark has to
	/// sit on an instruction.
	Instruction *allocation (BasicBlock *block, MonoClass *klass)
	{
		IRBuilder<> b (block->getTerminator ());
		Instruction *made = b.CreateLoad (b.getPtrTy (), caller->getArg (1));

		if (klass != nullptr)
			mark_exact_class (*made, klass);

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

/// A function that carries a receiver around a loop, the way the LINQ
/// iterator chain that motivated this walk does. The value the loop
/// dispatches on and the value it carries to the next round name each other.
///
///     entry -> header: %carried = phi [ null, entry ], [ %current, latch ]
///                      br (%carried == null) ? make : reuse
///     make:   %fresh = <marked allocation>; br merge
///     reuse:  br merge
///     merge:  %current = phi [ %fresh, make ], [ %carried, reuse ]
///             br %again ? latch : exit
///     latch:  br header
///     exit:   ret %current
///
/// `%carried` and `%current` are the mutual cycle: `%carried`'s non-null
/// incoming is `%current`, and `%current`'s incoming for the "already have
/// one" arm is `%carried`.
struct CycleModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	PHINode *carried = nullptr;
	PHINode *current = nullptr;

	explicit CycleModule (MonoClass *fresh_klass)
	{
		module = std::make_unique<Module> ("cycle", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *i1 = Type::getInt1Ty (*context);

		caller = Function::Create (FunctionType::get (ptr, { i1, ptr }, false),
		                           GlobalValue::ExternalLinkage, "caller", module.get ());

		auto *entry = BasicBlock::Create (*context, "entry", caller);
		auto *header = BasicBlock::Create (*context, "header", caller);
		auto *make = BasicBlock::Create (*context, "make", caller);
		auto *reuse = BasicBlock::Create (*context, "reuse", caller);
		auto *merge = BasicBlock::Create (*context, "merge", caller);
		auto *latch = BasicBlock::Create (*context, "latch", caller);
		auto *exit = BasicBlock::Create (*context, "exit", caller);

		IRBuilder<> (entry).CreateBr (header);

		IRBuilder<> hb (header);
		carried = hb.CreatePHI (ptr, 2);
		Value *is_null = hb.CreateICmpEQ (carried, ConstantPointerNull::get (
		                                                cast<PointerType> (ptr)));
		hb.CreateCondBr (is_null, make, reuse);

		IRBuilder<> mb (make);
		Instruction *fresh = mb.CreateLoad (ptr, caller->getArg (1));

		if (fresh_klass != nullptr)
			mark_exact_class (*fresh, fresh_klass);

		mb.CreateBr (merge);

		IRBuilder<> (reuse).CreateBr (merge);

		IRBuilder<> gb (merge);
		current = gb.CreatePHI (ptr, 2);
		current->addIncoming (fresh, make);
		current->addIncoming (carried, reuse);
		gb.CreateCondBr (caller->getArg (0), latch, exit);

		IRBuilder<> (latch).CreateBr (header);
		carried->addIncoming (ConstantPointerNull::get (cast<PointerType> (ptr)), entry);
		carried->addIncoming (current, latch);

		IRBuilder<> (exit).CreateRet (current);
	}
};

TEST (OperandClassTest, PhiWithAgreeingExactArmsAnswersTheClass)
{
	MergeModule m;
	Instruction *left = m.allocation (m.left, classX);
	Instruction *right = m.allocation (m.right, classX);
	PHINode *phi = m.joined (left, right);

	auto [klass, exact] = operand_class (phi, *m.caller);

	EXPECT_EQ (klass, classX);
	EXPECT_TRUE (exact);
	EXPECT_EQ (exact_class (phi, *m.caller), classX);
}

TEST (OperandClassTest, PhiWithDisagreeingArmsAnswersNoClass)
{
	MergeModule m;
	Instruction *left = m.allocation (m.left, classX);
	Instruction *right = m.allocation (m.right, classY);
	PHINode *phi = m.joined (left, right);

	EXPECT_EQ (operand_class (phi, *m.caller).first, nullptr);
	EXPECT_EQ (exact_class (phi, *m.caller), nullptr);
}

TEST (OperandClassTest, SelectWithAgreeingArmsAnswersTheClass)
{
	MergeModule m;
	Instruction *left = m.allocation (m.left, classX);
	Instruction *right = m.allocation (m.right, classX);

	IRBuilder<> b (m.left->getTerminator ());
	Value *selected = b.CreateSelect (m.caller->getArg (0), left, right);

	auto [klass, exact] = operand_class (selected, *m.caller);

	EXPECT_EQ (klass, classX);
	EXPECT_TRUE (exact);
}

/// The default entry point never resolves a merge through a null arm: an
/// `isinst` reads a null answer itself, so guessing a class here would be a
/// wrong answer rather than a missing optimization.
TEST (OperandClassTest, OperandClassAnswersNoClassAcrossANullArm)
{
	MergeModule m;
	Instruction *left = m.allocation (m.left, classX);
	Value *null_value = ConstantPointerNull::get (cast<PointerType> (m.caller->getArg (1)->getType ()));
	PHINode *phi = m.joined (left, null_value);

	EXPECT_EQ (operand_class (phi, *m.caller).first, nullptr);
}

/// `exact_class ()` is for a caller about to dereference the value, and a
/// null arm faults before that dereference runs. So it reads past the null
/// arm to the class every other arm agrees on.
TEST (OperandClassTest, ExactClassIgnoresANullArm)
{
	MergeModule m;
	Instruction *left = m.allocation (m.left, classX);
	Value *null_value = ConstantPointerNull::get (cast<PointerType> (m.caller->getArg (1)->getType ()));
	PHINode *phi = m.joined (left, null_value);

	EXPECT_EQ (exact_class (phi, *m.caller), classX);
}

/// The mutual cycle a loop-carried receiver forms (`%carried` and `%current`
/// name each other) must not defeat the walk. The null `%carried` starts
/// with must not stop it from answering either, because the header tests
/// for null before the value can reach a dereference.
TEST (OperandClassTest, ExactClassResolvesAMutualCycleThroughNull)
{
	CycleModule m (classX);

	EXPECT_EQ (exact_class (m.current, *m.caller), classX);
	EXPECT_EQ (exact_class (m.carried, *m.caller), classX);
}

/// `operand_class ()` does not get the same answer for the same cycle,
/// because it does not ignore the null `%carried` starts with. The null
/// incoming settles `%carried` at no class, which settles `%current` at no
/// class too.
TEST (OperandClassTest, OperandClassDoesNotResolveTheSameCycle)
{
	CycleModule m (classX);

	EXPECT_EQ (operand_class (m.current, *m.caller).first, nullptr);
}

/// A cycle carrying two different classes must not answer either one, with
/// or without the null rule relaxed.
TEST (OperandClassTest, ExactClassRefusesACycleThatDisagrees)
{
	CycleModule m (classY);

	IRBuilder<> b (m.caller->getEntryBlock ().getTerminator ());
	Instruction *other = b.CreateLoad (PointerType::get (*m.context, 0), m.caller->getArg (1));
	mark_exact_class (*other, classX);
	m.carried->setIncomingValueForBlock (&m.caller->getEntryBlock (), other);

	EXPECT_EQ (exact_class (m.current, *m.caller), nullptr);
}

/// A chain of phis deep enough to exceed the walk's budget answers no class
/// rather than pay for the whole chain. Each phi's two incoming edges carry
/// the same upstream phi, which doubles the walk's work at every level.
/// Only the shared budget keeps that doubling from costing exponential time.
TEST (OperandClassTest, ExceedingTheWalkBudgetAnswersNoClass)
{
	LLVMContext context;
	Module module ("chain", context);
	Type *ptr = PointerType::get (context, 0);

	Function *caller = Function::Create (
		FunctionType::get (ptr, { Type::getInt1Ty (context), ptr }, false),
		GlobalValue::ExternalLinkage, "caller", &module);

	auto *entry = BasicBlock::Create (context, "entry", caller);
	Instruction *made = IRBuilder<> (entry).CreateLoad (ptr, caller->getArg (1));
	mark_exact_class (*made, classX);

	Value *previous = made;
	BasicBlock *previous_block = entry;

	// One level past the budget's 24 is enough to make sure the walk answers
	// rather than paying for the whole chain.
	for (unsigned level = 0; level < 25; ++level) {
		auto *left = BasicBlock::Create (context, "left", caller);
		auto *right = BasicBlock::Create (context, "right", caller);
		auto *join = BasicBlock::Create (context, "join", caller);

		IRBuilder<> (previous_block).CreateCondBr (caller->getArg (0), left, right);
		IRBuilder<> (left).CreateBr (join);
		IRBuilder<> (right).CreateBr (join);

		IRBuilder<> jb (join);
		PHINode *phi = jb.CreatePHI (ptr, 2);
		phi->addIncoming (previous, left);
		phi->addIncoming (previous, right);

		previous = phi;
		previous_block = join;
	}

	IRBuilder<> (previous_block).CreateRet (previous);

	EXPECT_EQ (exact_class (previous, *caller), nullptr);
}

/// A function with one call to the object-allocation builtin, the vtable
/// operand a global marked with \p klass and no other class mark on the call.
///
/// `changeToInvokeAndSplitBasicBlock ()` builds an `InvokeInst` from a
/// `CallInst` the same way, keeping the same operands and copying no
/// metadata, so a plain call answers the same question the invoke it can
/// become would.
struct AllocationModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	CallInst *site = nullptr;

	AllocationModule (MonoClass *klass, StringRef decl_name)
	{
		module = std::make_unique<Module> ("alloc", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *word = Type::getInt64Ty (*context);

		Function *decl = Function::Create (
			FunctionType::get (ptr, { ptr, word, ptr }, false),
			GlobalValue::ExternalLinkage, decl_name, module.get ());

		auto *vtable = new GlobalVariable (*module, Type::getInt8Ty (*context), false,
		                                   GlobalValue::ExternalLinkage, nullptr, "vtable");
		mark_class_reference (*vtable, klass);

		caller = Function::Create (FunctionType::get (ptr, {}, false),
		                           GlobalValue::ExternalLinkage, "caller", module.get ());

		IRBuilder<> b (BasicBlock::Create (*context, "entry", caller));

		site = b.CreateCall (
			decl, { vtable, ConstantInt::get (word, 64), ConstantPointerNull::get (
				                                       cast<PointerType> (ptr)) });
		b.CreateRet (site);
	}
};

TEST (OperandClassTest, AllocationSiteAnswersItsVtableClassWithNoMark)
{
	mono::test::init_runtime ();

	AllocationModule m (mono_defaults.object_class, alloc_object_name);
	auto [klass, exact] = operand_class (m.site, *m.caller);

	EXPECT_EQ (klass, mono_defaults.object_class);
	EXPECT_TRUE (exact);
}

/// The kept form names the same allocation as the ordinary one and answers
/// its class the same way.
TEST (OperandClassTest, KeptAllocationSiteAnswersItsVtableClassWithNoMark)
{
	mono::test::init_runtime ();

	AllocationModule m (mono_defaults.object_class, alloc_object_kept_name);

	EXPECT_EQ (operand_class (m.site, *m.caller).first, mono_defaults.object_class);
}

/// A marshal-by-ref class's allocator can answer with a transparent proxy
/// instead of an instance of the class it was asked to allocate, so the
/// vtable operand must not settle the class here the way it does above.
TEST (OperandClassTest, AllocationSiteRefusesAMarshalByRefClass)
{
	mono::test::init_runtime ();

	MonoClass *klass = mono_class_from_name (mono_get_corlib (), "System", "MarshalByRefObject");
	ASSERT_NE (klass, nullptr);

	AllocationModule m (klass, alloc_object_name);

	EXPECT_EQ (operand_class (m.site, *m.caller).first, nullptr);
}

} // namespace
} // namespace test
} // namespace mono
