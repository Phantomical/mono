/*
 * Tests for StripVTableSnapshotPass, which takes a class's vtable back from the
 * constant a compile read to the symbol the link resolves.
 *
 * The layout comes from MonoVTable, so these name mono headers. They need no
 * runtime under them.
 */

#include "passes/vtable-snapshot.hpp"

#include "mono/metadata/abi-details.h"
#include "mono/metadata/class-internals.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
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

constexpr uint32_t test_slots = 4;

/// A module holding one vtable snapshot and a reader of it.
struct SnapshotModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	GlobalVariable *snapshot = nullptr;
	Function *reader = nullptr;

	SnapshotModule ()
	{
		module = std::make_unique<Module> ("snapshots", *context);

		Type *laid_out = vtable_snapshot_type (*module, test_slots);

		snapshot = new GlobalVariable (*module, laid_out, /*isConstant=*/false,
		                               GlobalValue::ExternalLinkage,
		                               Constant::getNullValue (laid_out),
		                               "mono_vtable_Some.Class@0x1234");
		mark_vtable_snapshot (*snapshot);

		Type *ptr = PointerType::get (*context, 0);

		reader = Function::Create (FunctionType::get (ptr, {}, false),
		                           GlobalValue::ExternalLinkage, "reader",
		                           module.get ());
	}

	/// Reads the snapshot \p offset bytes in, and returns the value.
	void read_at (uint64_t offset)
	{
		IRBuilder<> b (BasicBlock::Create (*context, "entry", reader));
		Value *at = b.CreateGEP (b.getInt8Ty (), snapshot, b.getInt64 (offset));

		b.CreateRet (b.CreateAlignedLoad (PointerType::get (*context, 0), at,
		                                  Align (8)));
	}

	void strip ()
	{
		ModuleAnalysisManager mam;

		StripVTableSnapshotPass ().run (*module, mam);
	}
};

} // namespace

TEST (VTableSnapshot, TheLayoutIsMonoVTablesOwn)
{
	LLVMContext context;
	Module m ("layout", context);
	Type *laid_out = vtable_snapshot_type (m, test_slots);

	EXPECT_EQ (m.getDataLayout ().getTypeAllocSize (laid_out),
	           MONO_SIZEOF_VTABLE + test_slots * sizeof (gpointer));
}

TEST (VTableSnapshot, TheInventoryIsTheFieldsFixedWhenTheVtableIsBuilt)
{
	// mono_class_create_runtime_vtable () writes each of these once, while it
	// builds the vtable, and nothing writes them again.
	EXPECT_TRUE (vtable_snapshot_states (MONO_STRUCT_OFFSET (MonoVTable, klass),
	                                     test_slots));
	EXPECT_TRUE (vtable_snapshot_states (MONO_STRUCT_OFFSET (MonoVTable, rank),
	                                     test_slots));
	EXPECT_TRUE (vtable_snapshot_states (MONO_STRUCT_OFFSET (MonoVTable, type),
	                                     test_slots));

	// The collector writes gc_bits, and the three fields beside it share its
	// storage unit. The rest here is written after the vtable exists.
	EXPECT_FALSE (vtable_snapshot_states (
		MONO_STRUCT_OFFSET (MonoVTable, imt_collisions_bitmap), test_slots));
	EXPECT_FALSE (vtable_snapshot_states (
		MONO_STRUCT_OFFSET (MonoVTable, runtime_generic_context), test_slots));

	// Every slot the class has, so a dispatch on a settled receiver folds to
	// the callee. One past the last is not: it is the static field block, or
	// nothing at all.
	EXPECT_TRUE (vtable_snapshot_states (MONO_STRUCT_OFFSET (MonoVTable, vtable),
	                                     test_slots));
	EXPECT_TRUE (vtable_snapshot_states (
		MONO_STRUCT_OFFSET (MonoVTable, vtable)
			+ (test_slots - 1) * sizeof (gpointer),
		test_slots));
	EXPECT_FALSE (vtable_snapshot_states (MONO_STRUCT_OFFSET (MonoVTable, vtable)
	                                              + test_slots * sizeof (gpointer),
	                                      test_slots));

	// A snapshot with no slot array states none of them.
	EXPECT_FALSE (vtable_snapshot_states (MONO_STRUCT_OFFSET (MonoVTable, vtable), 0));
}

TEST (VTableSnapshot, TheInitializerStatesTheClassAsASymbol)
{
	SnapshotModule m;
	LLVMContext &c = *m.context;
	auto *klass = new GlobalVariable (*m.module, Type::getInt8Ty (c), false,
	                                  GlobalValue::ExternalLinkage, nullptr,
	                                  "mono_class_Some.Class@0x1234");
	VTableFacts facts;

	facts.klass = klass;
	facts.rank = 3;

	Constant *held = vtable_snapshot_init (*m.module, facts);
	const DataLayout &dl = m.module->getDataLayout ();
	auto *laid_out = cast<StructType> (held->getType ());
	const StructLayout *where = dl.getStructLayout (laid_out);

	// A type test compares the class word against the same symbol, so the two
	// have to be one value for the comparison to fold.
	unsigned at = where->getElementContainingOffset (
		MONO_STRUCT_OFFSET (MonoVTable, klass));

	EXPECT_EQ (held->getAggregateElement (at), klass);

	at = where->getElementContainingOffset (MONO_STRUCT_OFFSET (MonoVTable, rank));
	EXPECT_EQ (held->getAggregateElement (at), ConstantInt::get (Type::getInt8Ty (c), 3));
}

TEST (VTableSnapshot, ASnapshotGoesBackToTheSymbolUnderItsOwnName)
{
	SnapshotModule m;

	m.read_at (MONO_STRUCT_OFFSET (MonoVTable, klass));
	ASSERT_TRUE (m.snapshot->hasInitializer ());

	m.strip ();

	// The same global, so every use the optimization left standing is now a use
	// of the symbol the link resolves.
	GlobalVariable *left = m.module->getGlobalVariable ("mono_vtable_Some.Class@0x1234");

	ASSERT_EQ (left, m.snapshot);
	EXPECT_FALSE (left->hasInitializer ());
	EXPECT_FALSE (left->isConstant ());
	EXPECT_FALSE (left->hasMetadata (vtable_snapshot_metadata));
	EXPECT_EQ (left->getLinkage (), GlobalValue::ExternalLinkage);
	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
}

TEST (VTableSnapshot, AModuleWithNoSnapshotIsLeftAlone)
{
	SnapshotModule m;

	m.read_at (MONO_STRUCT_OFFSET (MonoVTable, klass));
	m.strip ();
	EXPECT_FALSE (errorToBool (vtable_snapshots_stripped (*m.module)));

	// The check behind the sweep only fires on a snapshot the sweep never saw.
	mark_vtable_snapshot (*m.snapshot);
	EXPECT_TRUE (errorToBool (vtable_snapshots_stripped (*m.module)));
}

TEST (VTableSnapshotDeathTest, AReadOfAWithheldFieldStopsTheProcess)
{
	SnapshotModule m;

	m.read_at (MONO_STRUCT_OFFSET (MonoVTable, runtime_generic_context));

	EXPECT_DEATH (m.strip (), "does not state");
}

} // namespace test
} // namespace mono
