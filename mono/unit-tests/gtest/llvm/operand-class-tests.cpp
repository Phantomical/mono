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

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ModRef.h>

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

/// A function with one allocation site real enough for `allocation_class ()`
/// to answer it, a field at a fixed offset off that allocation, and the
/// pieces a test needs to store into and load back from that field.
///
/// A store is legal any time after the allocation and before the load that
/// reads it back: `matching_field_stores ()` gathers every store to the
/// field by walking uses, not by tracing which one a particular path
/// executes, so two stores in the same straight line stand for two stores on
/// two different paths as far as the walk is concerned.
struct FieldModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	CallInst *base = nullptr;
	Value *field_address = nullptr;
	BasicBlock *entry = nullptr;

	static constexpr int64_t field_offset = 8;

	explicit FieldModule (MonoClass *base_klass)
	{
		module = std::make_unique<Module> ("field", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *word = Type::getInt64Ty (*context);

		Function *decl = Function::Create (
			FunctionType::get (ptr, { ptr, word, ptr }, false),
			GlobalValue::ExternalLinkage, alloc_object_name, module.get ());

		auto *vtable = new GlobalVariable (*module, Type::getInt8Ty (*context), false,
		                                   GlobalValue::ExternalLinkage, nullptr, "vtable");
		mark_class_reference (*vtable, base_klass);

		caller = Function::Create (
			FunctionType::get (ptr, { PointerType::get (*context, 0) }, false),
			GlobalValue::ExternalLinkage, "caller", module.get ());

		entry = BasicBlock::Create (*context, "entry", caller);

		IRBuilder<> b (entry);

		base = b.CreateCall (
			decl, { vtable, ConstantInt::get (word, 64),
			        ConstantPointerNull::get (cast<PointerType> (ptr)) });
		field_address = b.CreateGEP (Type::getInt8Ty (*context), base,
		                             { ConstantInt::get (word, field_offset) });
	}

	/// A store of \p value into the field, appended to \p block.
	void store_field (BasicBlock *block, Value *value)
	{
		IRBuilder<> (block).CreateStore (value, field_address);
	}

	/// A load in \p block, marked as an instance of \p klass when \p klass is
	/// given, standing for a value a test stores into the field. This reuses
	/// `MergeModule::allocation ()`'s reason for a load rather than a
	/// constant: a mark has to sit on an instruction.
	Instruction *marked_value (BasicBlock *block, MonoClass *klass)
	{
		IRBuilder<> b (block);
		Instruction *made = b.CreateLoad (b.getPtrTy (), caller->getArg (0));

		if (klass != nullptr)
			mark_exact_class (*made, klass);

		return made;
	}

	/// Ends \p block with a load of the field and a return of it.
	LoadInst *load_and_return (BasicBlock *block)
	{
		IRBuilder<> b (block);
		LoadInst *load = b.CreateLoad (b.getPtrTy (), field_address);

		b.CreateRet (load);

		return load;
	}
};

/// The split this whole rule stands on: the allocation starts zero-filled,
/// so a path where the load runs before the store is still live, and
/// `operand_class ()` must answer no class for it the way it would for an
/// `isinst` reading this field. `exact_class ()` is for a caller that
/// dereferences right after, so the null path faults there before it
/// matters, and that entry reads past it to the one store's class.
TEST (OperandClassTest, LoadWithOneStoreSplitsOperandClassFromExactClass)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *value = m.marked_value (m.entry, classX);

	m.store_field (m.entry, value);
	LoadInst *load = m.load_and_return (m.entry);

	EXPECT_EQ (operand_class (load, *m.caller).first, nullptr);
	EXPECT_EQ (exact_class (load, *m.caller), classX);
}

/// The class every store agrees on settles the load even where the objects
/// those stores write are themselves different, because the rule is about
/// what class the field holds, not which object. This reads through
/// `exact_class ()`, because `operand_class ()` answers no class here too:
/// the field's zero-filled initial value is still one of the arms.
TEST (OperandClassTest, LoadAnswersWhereTwoStoresNameDifferentObjectsOfTheSameClass)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *first = m.marked_value (m.entry, classX);
	Instruction *second = m.marked_value (m.entry, classX);

	m.store_field (m.entry, first);
	m.store_field (m.entry, second);
	LoadInst *load = m.load_and_return (m.entry);

	EXPECT_EQ (exact_class (load, *m.caller), classX);
}

/// Two stores that disagree settle the merge at no class before the null arm
/// even enters into it, so both entry points refuse here.
TEST (OperandClassTest, LoadAnswersNoClassWhereTwoStoresDisagree)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *first = m.marked_value (m.entry, classX);
	Instruction *second = m.marked_value (m.entry, classY);

	m.store_field (m.entry, first);
	m.store_field (m.entry, second);
	LoadInst *load = m.load_and_return (m.entry);

	EXPECT_EQ (operand_class (load, *m.caller).first, nullptr);
	EXPECT_EQ (exact_class (load, *m.caller), nullptr);
}

/// The escape guard is what this rule stands on: passing the allocation to a
/// call hands its address to code this walk cannot read, so the answer must
/// go to no class even though the one store still in sight names a settled
/// class.
TEST (OperandClassTest, LoadAnswersNoClassWhereTheAllocationEscapesToACall)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *value = m.marked_value (m.entry, classX);

	m.store_field (m.entry, value);

	Function *sink = Function::Create (
		FunctionType::get (Type::getVoidTy (*m.context), { m.base->getType () }, false),
		GlobalValue::ExternalLinkage, "sink", m.module.get ());
	IRBuilder<> (m.entry).CreateCall (sink, { m.base });

	LoadInst *load = m.load_and_return (m.entry);

	EXPECT_EQ (operand_class (load, *m.caller).first, nullptr);
	EXPECT_EQ (exact_class (load, *m.caller), nullptr);
}

/// The same guard covers the allocation escaping as the value a store writes
/// into another object, rather than as the address a store writes through.
TEST (OperandClassTest, LoadAnswersNoClassWhereTheAllocationIsStoredIntoAnotherObject)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *value = m.marked_value (m.entry, classX);

	m.store_field (m.entry, value);
	IRBuilder<> (m.entry).CreateStore (m.base, m.caller->getArg (0));

	LoadInst *load = m.load_and_return (m.entry);

	EXPECT_EQ (operand_class (load, *m.caller).first, nullptr);
	EXPECT_EQ (exact_class (load, *m.caller), nullptr);
}

/// A declaration shaped like the write barrier's own
/// (`gc_barrier_decl ()`, `passes/gc-barrier.cpp`): one pointer parameter
/// carrying whatever \p captures and \p effects a test asks for.
Function *
declare_sink (Module &m, Type *param, bool captures, MemoryEffects effects)
{
	Function *decl = Function::Create (
		FunctionType::get (Type::getVoidTy (m.getContext ()), { param }, false),
		GlobalValue::ExternalLinkage, "sink", &m);

	if (!captures)
		decl->addParamAttr (
			0, Attribute::getWithCaptureInfo (m.getContext (), CaptureInfo::none ()));

	decl->setMemoryEffects (effects);
	return decl;
}

/// A call the write barrier's own attributes mark as safe must not stop the
/// walk from reaching the store behind it.
///
/// Read the effects off `getMemoryEffects ()` rather than off a `readonly`
/// parameter attribute. `CallBase::onlyReadsMemory (OpNo)` answers only the
/// parameter attribute, and says nothing about the callee's own memory
/// effects. A callee that states argument memory is read-only at the
/// function level, the way the barrier declaration does, must still count.
TEST (OperandClassTest, LoadAnswersWhereTheFieldAddressReachesACallThatCannotWriteOrCapture)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *value = m.marked_value (m.entry, classX);

	m.store_field (m.entry, value);

	Function *sink = declare_sink (*m.module, m.field_address->getType (), /*captures=*/false,
	                               MemoryEffects::argMemOnly (ModRefInfo::Ref));
	IRBuilder<> (m.entry).CreateCall (sink, { m.field_address });

	LoadInst *load = m.load_and_return (m.entry);

	EXPECT_EQ (exact_class (load, *m.caller), classX);
}

/// A callee whose memory effects say it can write argument memory is refused
/// even though it does not keep the pointer. Not capturing it only bounds how
/// long the callee can hold it, not whether the call itself writes through it.
TEST (OperandClassTest, LoadAnswersNoClassWhereTheCallMayWriteTheFieldAddress)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *value = m.marked_value (m.entry, classX);

	m.store_field (m.entry, value);

	Function *sink = declare_sink (*m.module, m.field_address->getType (), /*captures=*/false,
	                               MemoryEffects::argMemOnly (ModRefInfo::ModRef));
	IRBuilder<> (m.entry).CreateCall (sink, { m.field_address });

	LoadInst *load = m.load_and_return (m.entry);

	EXPECT_EQ (operand_class (load, *m.caller).first, nullptr);
	EXPECT_EQ (exact_class (load, *m.caller), nullptr);
}

/// A callee that can keep the field address is refused even though it never
/// writes through it. A kept pointer can be written back to later, from code
/// this walk never sees.
TEST (OperandClassTest, LoadAnswersNoClassWhereTheCallMayCaptureTheFieldAddress)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *value = m.marked_value (m.entry, classX);

	m.store_field (m.entry, value);

	Function *sink = declare_sink (*m.module, m.field_address->getType (), /*captures=*/true,
	                               MemoryEffects::argMemOnly (ModRefInfo::Ref));
	IRBuilder<> (m.entry).CreateCall (sink, { m.field_address });

	LoadInst *load = m.load_and_return (m.entry);

	EXPECT_EQ (operand_class (load, *m.caller).first, nullptr);
	EXPECT_EQ (exact_class (load, *m.caller), nullptr);
}

/// A pointer compare writes nothing and yields an `i1`, so it is not a route
/// by which the object is written, whether it compares the allocation itself
/// or one of its fields - both are exactly what the translator's own null
/// test emits ahead of a dereference.
TEST (OperandClassTest, LoadAnswersWhereTheAllocationOrAFieldIsCompared)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *value = m.marked_value (m.entry, classX);

	m.store_field (m.entry, value);

	Value *null_base = ConstantPointerNull::get (cast<PointerType> (m.base->getType ()));
	Value *null_field = ConstantPointerNull::get (cast<PointerType> (m.field_address->getType ()));

	IRBuilder<> (m.entry).CreateICmpEQ (m.base, null_base);
	IRBuilder<> (m.entry).CreateICmpEQ (m.field_address, null_field);

	LoadInst *load = m.load_and_return (m.entry);

	EXPECT_EQ (exact_class (load, *m.caller), classX);
}

/// `ptrtoint` stays refused: an integer can be turned back into a pointer and
/// written through, and this walk cannot see where that happens.
TEST (OperandClassTest, LoadAnswersNoClassWhereTheAllocationIsConvertedToAnInteger)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *value = m.marked_value (m.entry, classX);

	m.store_field (m.entry, value);
	IRBuilder<> (m.entry).CreatePtrToInt (m.base, Type::getInt64Ty (*m.context));

	LoadInst *load = m.load_and_return (m.entry);

	EXPECT_EQ (operand_class (load, *m.caller).first, nullptr);
	EXPECT_EQ (exact_class (load, *m.caller), nullptr);
}

/// A function that reads a field off a phi of two allocations of \p
/// base_klass, one made on each arm of a branch. `store_into ()` lets a test
/// write a marked value into one allocation's own field, right after it is
/// made.
struct PhiOfAllocationsFieldModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	CallInst *alloc1 = nullptr;
	CallInst *alloc2 = nullptr;
	BasicBlock *left = nullptr;
	LoadInst *load = nullptr;

	static constexpr int64_t field_offset = 8;

	explicit PhiOfAllocationsFieldModule (MonoClass *base_klass)
	{
		module = std::make_unique<Module> ("phi-field", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *word = Type::getInt64Ty (*context);

		Function *decl = Function::Create (
			FunctionType::get (ptr, { ptr, word, ptr }, false),
			GlobalValue::ExternalLinkage, alloc_object_name, module.get ());

		auto *vtable = new GlobalVariable (*module, Type::getInt8Ty (*context), false,
		                                   GlobalValue::ExternalLinkage, nullptr, "vtable");
		mark_class_reference (*vtable, base_klass);

		caller = Function::Create (
			FunctionType::get (ptr, { Type::getInt1Ty (*context), ptr }, false),
			GlobalValue::ExternalLinkage, "caller", module.get ());

		auto *entry = BasicBlock::Create (*context, "entry", caller);
		left = BasicBlock::Create (*context, "left", caller);
		auto *right = BasicBlock::Create (*context, "right", caller);
		auto *merge = BasicBlock::Create (*context, "merge", caller);

		IRBuilder<> (entry).CreateCondBr (caller->getArg (0), left, right);

		Value *null_val = ConstantPointerNull::get (cast<PointerType> (ptr));

		IRBuilder<> lb (left);
		alloc1 = lb.CreateCall (decl, { vtable, ConstantInt::get (word, 64), null_val });
		lb.CreateBr (merge);

		IRBuilder<> rb (right);
		alloc2 = rb.CreateCall (decl, { vtable, ConstantInt::get (word, 64), null_val });
		rb.CreateBr (merge);

		IRBuilder<> mb (merge);
		PHINode *phi = mb.CreatePHI (ptr, 2);
		phi->addIncoming (alloc1, left);
		phi->addIncoming (alloc2, right);
		Value *field_address =
			mb.CreateGEP (Type::getInt8Ty (*context), phi, { ConstantInt::get (word, field_offset) });
		load = mb.CreateLoad (ptr, field_address);
		mb.CreateRet (load);
	}

	/// Stores a value marked \p klass into \p alloc's own field, right after
	/// \p alloc is made.
	void store_into (CallInst *alloc, MonoClass *klass)
	{
		IRBuilder<> b (alloc->getNextNode ());
		Instruction *value = b.CreateLoad (b.getPtrTy (), caller->getArg (1));
		mark_exact_class (*value, klass);
		Value *addr = b.CreateGEP (Type::getInt8Ty (*context), alloc,
		                          { ConstantInt::get (Type::getInt64Ty (*context), field_offset) });
		b.CreateStore (value, addr);
	}
};

/// The base is a phi of two allocations, each storing an instance of the
/// same class into the field the caller reads back. Resolving the base
/// through the phi to both allocations, and reading each one's own field
/// store, is what `resolve_base_candidates ()` (`operand-class.cpp`) adds
/// over a single allocation base.
TEST (OperandClassTest, LoadThroughAPhiOfAllocationsAnswersTheClass)
{
	mono::test::init_runtime ();

	PhiOfAllocationsFieldModule m (mono_defaults.object_class);
	m.store_into (m.alloc1, classX);
	m.store_into (m.alloc2, classX);

	EXPECT_EQ (exact_class (m.load, *m.caller), classX);
}

/// One of the two candidates escapes - its address is stored into another
/// object, the same route `LoadAnswersNoClassWhereTheAllocationIsStoredIntoAnotherObject`
/// above gates for a single allocation. `resolve_base_candidates ()` runs
/// `field_stores_reaching ()` over every candidate it finds, so the escaping
/// one is what settles the whole answer at no class, not just its own arm.
TEST (OperandClassTest, LoadThroughAPhiOfAllocationsAnswersNoClassWhereOneEscapes)
{
	mono::test::init_runtime ();

	PhiOfAllocationsFieldModule m (mono_defaults.object_class);
	m.store_into (m.alloc1, classX);
	m.store_into (m.alloc2, classX);

	IRBuilder<> (m.left->getTerminator ()).CreateStore (m.alloc1, m.caller->getArg (1));

	EXPECT_EQ (operand_class (m.load, *m.caller).first, nullptr);
	EXPECT_EQ (exact_class (m.load, *m.caller), nullptr);
}

/// The two candidates store different classes into the field. Agreeing on
/// two different objects of the same class settles a merge; agreeing on
/// nothing must not.
TEST (OperandClassTest, LoadThroughAPhiOfAllocationsAnswersNoClassWhereTheyDisagree)
{
	mono::test::init_runtime ();

	PhiOfAllocationsFieldModule m (mono_defaults.object_class);
	m.store_into (m.alloc1, classX);
	m.store_into (m.alloc2, classY);

	EXPECT_EQ (operand_class (m.load, *m.caller).first, nullptr);
	EXPECT_EQ (exact_class (m.load, *m.caller), nullptr);
}

/// A function with two allocations, an outer one and an inner one, where the
/// inner's own address is the outer's one field store. Reading the inner's
/// own field back therefore needs a base resolved through a load - the
/// outer's field read - rather than through a phi or a select.
struct LoadAsBaseModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	Value *inner_field_address = nullptr;

	static constexpr int64_t outer_field_offset = 8;
	static constexpr int64_t inner_field_offset = 16;

	LoadAsBaseModule (MonoClass *outer_klass, MonoClass *inner_klass)
	{
		module = std::make_unique<Module> ("load-base", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *word = Type::getInt64Ty (*context);

		Function *decl = Function::Create (
			FunctionType::get (ptr, { ptr, word, ptr }, false),
			GlobalValue::ExternalLinkage, alloc_object_name, module.get ());

		auto *outer_vtable = new GlobalVariable (
			*module, Type::getInt8Ty (*context), false,
			GlobalValue::ExternalLinkage, nullptr, "outer_vtable");
		mark_class_reference (*outer_vtable, outer_klass);

		auto *inner_vtable = new GlobalVariable (
			*module, Type::getInt8Ty (*context), false,
			GlobalValue::ExternalLinkage, nullptr, "inner_vtable");
		mark_class_reference (*inner_vtable, inner_klass);

		caller = Function::Create (FunctionType::get (ptr, { ptr }, false),
		                          GlobalValue::ExternalLinkage, "caller", module.get ());

		auto *entry = BasicBlock::Create (*context, "entry", caller);
		IRBuilder<> b (entry);

		Value *null_val = ConstantPointerNull::get (cast<PointerType> (ptr));
		CallInst *outer = b.CreateCall (decl, { outer_vtable, ConstantInt::get (word, 64), null_val });
		CallInst *inner = b.CreateCall (decl, { inner_vtable, ConstantInt::get (word, 64), null_val });

		Value *outer_field_address = b.CreateGEP (
			Type::getInt8Ty (*context), outer, { ConstantInt::get (word, outer_field_offset) });
		b.CreateStore (inner, outer_field_address);

		Value *inner_read_back = b.CreateLoad (ptr, outer_field_address);
		inner_field_address = b.CreateGEP (
			Type::getInt8Ty (*context), inner_read_back, { ConstantInt::get (word, inner_field_offset) });
	}

	/// A load in \p block, marked as an instance of \p klass when \p klass is
	/// given, standing for a value a test stores into the inner allocation's
	/// own field.
	Instruction *marked_value (BasicBlock *block, MonoClass *klass)
	{
		IRBuilder<> b (block);
		Instruction *made = b.CreateLoad (b.getPtrTy (), caller->getArg (0));

		if (klass != nullptr)
			mark_exact_class (*made, klass);

		return made;
	}

	/// A store of \p value into the inner allocation's own field, appended to
	/// \p block.
	void store_field (BasicBlock *block, Value *value)
	{
		IRBuilder<> (block).CreateStore (value, inner_field_address);
	}

	/// Ends \p block with a load of the inner allocation's own field and a
	/// return of it.
	LoadInst *load_and_return (BasicBlock *block)
	{
		IRBuilder<> b (block);
		LoadInst *load = b.CreateLoad (b.getPtrTy (), inner_field_address);

		b.CreateRet (load);

		return load;
	}
};

/// The base of the field this reads is itself a load - the outer
/// allocation's own field, which the inner allocation's address was stored
/// into once. `resolve_base_candidates ()` reads that store back through
/// `matching_field_stores ()`, the mutual recursion the two functions form,
/// and does reach the inner allocation as a candidate.
///
/// The answer is still no class, and this is the boundary of what the
/// generalization can reach rather than a bug in it. The inner allocation
/// is discoverable through the outer's field only because its own address
/// was written there - `store inner, outer_field_address` is a store whose
/// *value* operand is the candidate, the same shape
/// `LoadAnswersNoClassWhereTheAllocationIsStoredIntoAnotherObject` above
/// gates for a direct base. `field_stores_reaching ()` refuses a candidate
/// on exactly that shape, so a candidate this walk can only reach by
/// reading it back out of a field can never itself clear the escape check:
/// the read is proof the candidate was written out to memory first.
TEST (OperandClassTest, LoadThroughALoadAsBaseAnswersNoClassBecauseTheDiscoveryStoreEscapes)
{
	mono::test::init_runtime ();

	LoadAsBaseModule m (mono_defaults.object_class, mono_defaults.object_class);
	BasicBlock &entry = m.caller->getEntryBlock ();
	Instruction *value = m.marked_value (&entry, classX);

	m.store_field (&entry, value);
	LoadInst *load = m.load_and_return (&entry);

	EXPECT_EQ (operand_class (load, *m.caller).first, nullptr);
	EXPECT_EQ (exact_class (load, *m.caller), nullptr);
}

/// A function that carries a receiver around a loop the way `CycleModule`
/// above does, but returns a field of the carried value rather than the
/// value itself, so resolving that field's base has to terminate on the same
/// mutual cycle `resolve_base_candidates ()` walks.
struct CyclicFieldModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	CallInst *fresh = nullptr;
	LoadInst *load = nullptr;

	static constexpr int64_t field_offset = 8;

	explicit CyclicFieldModule (MonoClass *base_klass)
	{
		module = std::make_unique<Module> ("cyclic-field", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *i1 = Type::getInt1Ty (*context);
		Type *word = Type::getInt64Ty (*context);

		Function *decl = Function::Create (
			FunctionType::get (ptr, { ptr, word, ptr }, false),
			GlobalValue::ExternalLinkage, alloc_object_name, module.get ());

		auto *vtable = new GlobalVariable (*module, Type::getInt8Ty (*context), false,
		                                   GlobalValue::ExternalLinkage, nullptr, "vtable");
		mark_class_reference (*vtable, base_klass);

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
		PHINode *carried = hb.CreatePHI (ptr, 2);
		Value *is_null = hb.CreateICmpEQ (
			carried, ConstantPointerNull::get (cast<PointerType> (ptr)));
		hb.CreateCondBr (is_null, make, reuse);

		IRBuilder<> mb (make);
		fresh = mb.CreateCall (
			decl, { vtable, ConstantInt::get (word, 64),
			        ConstantPointerNull::get (cast<PointerType> (ptr)) });
		mb.CreateBr (merge);

		IRBuilder<> (reuse).CreateBr (merge);

		IRBuilder<> gb (merge);
		PHINode *current = gb.CreatePHI (ptr, 2);
		current->addIncoming (fresh, make);
		current->addIncoming (carried, reuse);
		gb.CreateCondBr (caller->getArg (0), latch, exit);

		IRBuilder<> (latch).CreateBr (header);
		carried->addIncoming (ConstantPointerNull::get (cast<PointerType> (ptr)), entry);
		carried->addIncoming (current, latch);

		IRBuilder<> eb (exit);
		Value *field_address = eb.CreateGEP (
			Type::getInt8Ty (*context), current, { ConstantInt::get (word, field_offset) });
		load = eb.CreateLoad (ptr, field_address);
		eb.CreateRet (load);
	}

	/// Stores a value marked \p klass into the fresh allocation's own field,
	/// right after it is made.
	void store_field (MonoClass *klass)
	{
		IRBuilder<> b (fresh->getNextNode ());
		Instruction *value = b.CreateLoad (b.getPtrTy (), caller->getArg (1));
		mark_exact_class (*value, klass);
		Value *addr = b.CreateGEP (Type::getInt8Ty (*context), fresh,
		                          { ConstantInt::get (Type::getInt64Ty (*context), field_offset) });
		b.CreateStore (value, addr);
	}
};

/// The load's base is a phi that is itself part of the mutual cycle
/// `ExactClassResolvesAMutualCycleThroughNull` above gates for a bare
/// allocation. `resolve_base_candidates ()` must terminate on that cycle,
/// the same way `walk_operand_class ()` does, rather than loop forever
/// chasing the phi's own back edge.
TEST (OperandClassTest, LoadThroughACyclicPhiBaseTerminatesAndAnswers)
{
	mono::test::init_runtime ();

	CyclicFieldModule m (mono_defaults.object_class);
	m.store_field (classX);

	EXPECT_EQ (exact_class (m.load, *m.caller), classX);
}

/*
 * Below is `field_load_values ()`, the same walk `operand_class ()` runs over
 * a field load, exported for a caller that wants the values a store can leave
 * rather than the class they agree on. `FieldModule` is reused from above:
 * these tests answer with the store's own value, so `marked_value ()`'s class
 * mark plays no part in them.
 */

TEST (OperandClassTest, FieldLoadValuesAnswersTheOneStore)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *value = m.marked_value (m.entry, nullptr);

	m.store_field (m.entry, value);
	LoadInst *load = m.load_and_return (m.entry);

	FieldValues got = field_load_values (*load);

	ASSERT_EQ (got.values.size (), 1u);
	EXPECT_EQ (got.values[0], value);
	EXPECT_TRUE (got.complete);
}

/// Two stores of two different values both answer, the same way
/// `matching_field_stores ()` gathers every store to the field rather than
/// the one a particular path executes.
TEST (OperandClassTest, FieldLoadValuesAnswersEveryDistinctStore)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *first = m.marked_value (m.entry, nullptr);
	Instruction *second = m.marked_value (m.entry, nullptr);

	m.store_field (m.entry, first);
	m.store_field (m.entry, second);
	LoadInst *load = m.load_and_return (m.entry);

	FieldValues got = field_load_values (*load);

	ASSERT_EQ (got.values.size (), 2u);
	EXPECT_TRUE (is_contained (got.values, first));
	EXPECT_TRUE (is_contained (got.values, second));
}

/// The field's own zero-filled initial value is left out of the answer: a
/// field with one store still answers with that store alone, none of it a
/// null standing for the path where the load ran first.
TEST (OperandClassTest, FieldLoadValuesLeavesOutTheZeroFilledInitialValue)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *value = m.marked_value (m.entry, nullptr);

	m.store_field (m.entry, value);
	LoadInst *load = m.load_and_return (m.entry);

	FieldValues got = field_load_values (*load);

	for (const Value *v : got.values)
		EXPECT_FALSE (isa<ConstantPointerNull> (v));
}

/// A field no store ever reaches answers empty, the same way `operand_class
/// ()` answers no class for it.
TEST (OperandClassTest, FieldLoadValuesIsEmptyWhereNoStoreReachesTheField)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	LoadInst *load = m.load_and_return (m.entry);

	EXPECT_TRUE (field_load_values (*load).values.empty ());
}

/// `field_load_values ()` runs the walk under `ClassRule::guessed`, so an
/// allocation that escapes to a call still answers with the store this walk
/// found before the escape, marked incomplete rather than dropped. A caller
/// reading `complete` false knows the field can hold more than this set.
TEST (OperandClassTest, FieldLoadValuesAnswersIncompleteWhereTheAllocationEscapesToACall)
{
	mono::test::init_runtime ();

	FieldModule m (mono_defaults.object_class);
	Instruction *value = m.marked_value (m.entry, nullptr);

	m.store_field (m.entry, value);

	Function *sink = Function::Create (
		FunctionType::get (Type::getVoidTy (*m.context), { m.base->getType () }, false),
		GlobalValue::ExternalLinkage, "sink", m.module.get ());
	IRBuilder<> (m.entry).CreateCall (sink, { m.base });

	LoadInst *load = m.load_and_return (m.entry);

	FieldValues got = field_load_values (*load);

	ASSERT_EQ (got.values.size (), 1u);
	EXPECT_EQ (got.values[0], value);
	EXPECT_FALSE (got.complete);
}

} // namespace
} // namespace test
} // namespace mono
