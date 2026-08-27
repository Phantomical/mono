/*
 * Tests for lower_gc_barriers (), which writes a write barrier back as the card
 * the collector reads, and for fold_stack_barriers (), which takes a barrier
 * off a store into the frame.
 *
 * Pure LLVM. Each case stamps a layout of its own on the declaration, so the
 * three card shapes and the helper are all reachable whatever collector the
 * harness links.
 */

#include "passes/fold-barrier.hpp"
#include "passes/gc-barrier.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
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

/// Addresses that stand in for the collector's own. A test reads the shape the
/// lowering wrote, so what these point at is never dereferenced.
constexpr uintptr_t card_table_address = 0x40000000;
constexpr uintptr_t nursery_address = 0x50000000;
constexpr uintptr_t concurrent_flag_address = 0x60000000;

/// A module holding one reference store and the barrier beside it.
struct BarrierModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	CallInst *site = nullptr;
	StoreInst *store = nullptr;

	explicit BarrierModule (const GcBarrierLayout &layout)
	{
		module = std::make_unique<Module> ("barriers", *context);

		Type *ptr = PointerType::get (*context, 0);

		caller = Function::Create (FunctionType::get (Type::getVoidTy (*context),
		                                              { ptr, ptr }, false),
		                           GlobalValue::ExternalLinkage, "caller", module.get ());

		BasicBlock *entry = BasicBlock::Create (*context, "entry", caller);
		IRBuilder<> b (entry);
		Value *address = caller->getArg (0);
		Value *value = caller->getArg (1);

		store = b.CreateAlignedStore (value, address, Align (8));
		site = b.CreateCall (gc_barrier_decl (*module, layout), { address, value });
		b.CreateRetVoid ();
	}

	void lower () { lower_gc_barriers (*module); }

	std::string text () const
	{
		std::string printed;
		raw_string_ostream out (printed);

		module->print (out, nullptr);
		return printed;
	}

	unsigned count (StringRef needle) const
	{
		std::string text = this->text ();
		unsigned seen = 0;
		size_t at = 0;

		while ((at = text.find (needle.str (), at)) != std::string::npos) {
			++seen;
			at += needle.size ();
		}

		return seen;
	}
};

/// The layout of a collector that marks cards, with no concurrent major.
GcBarrierLayout
value_decides_layout ()
{
	GcBarrierLayout layout;

	layout.card_table = reinterpret_cast<void *> (card_table_address);
	layout.card_mask = 0x7fffff;
	layout.card_bits = 9;
	layout.nursery_start = reinterpret_cast<void *> (nursery_address);
	layout.nursery_bits = 22;
	layout.value_decides = true;
	return layout;
}

/// The same collector with a concurrent major, which reads a flag as well.
GcBarrierLayout
concurrent_layout ()
{
	GcBarrierLayout layout = value_decides_layout ();

	layout.value_decides = false;
	layout.concurrent_flag = reinterpret_cast<void *> (concurrent_flag_address);
	layout.concurrent_flag_size = 4;
	return layout;
}

TEST (GcBarrierTest, ACardCollectorTestsTheDestinationAndTheValue)
{
	BarrierModule m (value_decides_layout ());

	m.lower ();

	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
	EXPECT_EQ (m.module->getFunction (gc_barrier_name), nullptr);

	EXPECT_EQ (m.count ("%wb_target_is_old = icmp ne"), 1u) << m.text ();
	EXPECT_EQ (m.count ("%wb_value_is_young = icmp eq"), 1u) << m.text ();
	EXPECT_EQ (m.count ("store i8 1"), 1u) << m.text ();
	EXPECT_EQ (m.count ("@mono_gc_card_table"), 2u) << m.text ();

	// The collector keeps no flag to read, so nothing loads one.
	EXPECT_EQ (m.count ("load volatile"), 0u) << m.text ();
}

// A concurrent major collector wants a card under every old destination, and
// only while it marks. The value test alone would leave an old-to-old store off
// the card table for the rest of the program.
TEST (GcBarrierTest, AConcurrentMajorReadsItsFlagBesideTheValue)
{
	BarrierModule m (concurrent_layout ());

	m.lower ();

	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
	EXPECT_EQ (m.count ("load volatile i32, ptr @mono_gc_concurrent_collection_flag"),
	           1u)
		<< m.text ();
	EXPECT_EQ (m.count ("%wb_value_is_young = icmp eq"), 1u) << m.text ();
	EXPECT_EQ (m.count ("store i8 1"), 1u) << m.text ();
}

// A collector that keeps no flag can be marking at any moment, so the value
// says nothing and every old destination gets its card.
TEST (GcBarrierTest, AFlaglessConcurrentMajorMarksOnTheDestinationAlone)
{
	GcBarrierLayout layout = value_decides_layout ();

	layout.value_decides = false;

	BarrierModule m (layout);

	m.lower ();

	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
	EXPECT_EQ (m.count ("%wb_target_is_old = icmp ne"), 1u) << m.text ();
	EXPECT_EQ (m.count ("wb_value_is_young"), 0u) << m.text ();
	EXPECT_EQ (m.count ("store i8 1"), 1u) << m.text ();
}

// A collector that marks no cards has a helper of its own, and it takes the
// destination alone: the store beside the call already wrote the reference.
TEST (GcBarrierTest, ACollectorWithNoCardTableCallsItsHelper)
{
	BarrierModule m {GcBarrierLayout ()};

	m.lower ();

	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
	EXPECT_EQ (m.count ("call void @mono_gc_wbarrier_generic_nostore_internal(ptr %0)"),
	           1u)
		<< m.text ();
	EXPECT_EQ (m.count ("@mono_gc_card_table"), 0u) << m.text ();
}

// The lowering owes the store nothing: it is the caller's instruction and every
// arm leaves it where it stands, in front of the card.
TEST (GcBarrierTest, TheStoreSurvivesEveryLowering)
{
	for (const GcBarrierLayout &layout :
	     { GcBarrierLayout (), value_decides_layout (), concurrent_layout () }) {
		BarrierModule m (layout);

		m.lower ();

		ASSERT_FALSE (verifyModule (*m.module, &errs ()));

		BasicBlock &entry = m.caller->getEntryBlock ();
		bool found = false;

		for (Instruction &in : entry) {
			auto *store = dyn_cast<StoreInst> (&in);

			if (store == nullptr)
				continue;

			EXPECT_EQ (store->getPointerOperand (), m.caller->getArg (0));
			EXPECT_EQ (store->getValueOperand (), m.caller->getArg (1));
			found = true;
		}

		EXPECT_TRUE (found) << m.text ();
	}
}

// What the declaration claims about memory. The read of argument memory is what
// keeps the card mark behind the store, and the read and write of inaccessible
// memory covers the card table and the concurrent flag. Take the write out and
// DCE erases the barrier.
TEST (GcBarrierTest, TheDeclarationClaimsTheCardTableAndReadsItsArguments)
{
	BarrierModule m (concurrent_layout ());
	Function *decl = m.module->getFunction (gc_barrier_name);

	ASSERT_NE (decl, nullptr);

	MemoryEffects effects = decl->getMemoryEffects ();

	EXPECT_TRUE (decl->doesNotThrow ());
	EXPECT_TRUE (isRefSet (effects.getModRef (IRMemLocation::ArgMem)));
	EXPECT_FALSE (isModSet (effects.getModRef (IRMemLocation::ArgMem)));
	EXPECT_TRUE (isModSet (effects.getModRef (IRMemLocation::InaccessibleMem)));
	EXPECT_EQ (effects.getModRef (IRMemLocation::Other), ModRefInfo::NoModRef);
}

/// A module holding a caller with a local of its own.
struct StackModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	AllocaInst *local = nullptr;
	IRBuilder<> b;

	StackModule () : b (*context)
	{
		module = std::make_unique<Module> ("stack barriers", *context);

		Type *ptr = PointerType::get (*context, 0);

		caller = Function::Create (
			FunctionType::get (Type::getVoidTy (*context),
		                           { ptr, ptr, Type::getInt1Ty (*context) }, false),
			GlobalValue::ExternalLinkage, "caller", module.get ());

		b.SetInsertPoint (BasicBlock::Create (*context, "entry", caller));
		local = b.CreateAlloca (ArrayType::get (Type::getInt8Ty (*context), 32));
	}

	Value *value () { return caller->getArg (0); }

	Value *elsewhere () { return caller->getArg (1); }

	void store_through (Value *address)
	{
		b.CreateAlignedStore (value (), address, Align (8));
		b.CreateCall (gc_barrier_decl (*module, GcBarrierLayout ()),
		              { address, value () });
	}

	unsigned barriers () const
	{
		Function *decl = module->getFunction (gc_barrier_name);

		return decl == nullptr ? 0 : unsigned (decl->getNumUses ());
	}

	unsigned stores () const
	{
		unsigned seen = 0;

		for (Instruction &in : instructions (*caller))
			seen += isa<StoreInst> (&in) ? 1 : 0;

		return seen;
	}

	bool fold ()
	{
		b.CreateRetVoid ();

		bool changed = fold_stack_barriers (*caller);

		EXPECT_FALSE (verifyModule (*module, &errs ()));
		return changed;
	}
};

TEST (StackBarrierTest, ALocalsFieldNeedsNoCard)
{
	StackModule m;

	m.store_through (m.b.CreateConstInBoundsGEP1_32 (m.b.getInt8Ty (), m.local, 8));

	EXPECT_TRUE (m.fold ());
	EXPECT_EQ (m.barriers (), 0u);

	// The fold owes the store nothing: it is the reference the local holds.
	EXPECT_EQ (m.stores (), 1u);
}

TEST (StackBarrierTest, AnAddressFromOutsideKeepsItsBarrier)
{
	StackModule m;

	m.store_through (m.elsewhere ());

	EXPECT_FALSE (m.fold ());
	EXPECT_EQ (m.barriers (), 1u);
}

TEST (StackBarrierTest, ADestinationOfTwoObjectsKeepsItsBarrier)
{
	StackModule m;
	BasicBlock *on_stack = BasicBlock::Create (*m.context, "on_stack", m.caller);
	BasicBlock *on_heap = BasicBlock::Create (*m.context, "on_heap", m.caller);
	BasicBlock *join = BasicBlock::Create (*m.context, "join", m.caller);

	m.b.CreateCondBr (m.caller->getArg (2), on_stack, on_heap);
	m.b.SetInsertPoint (on_stack);
	m.b.CreateBr (join);
	m.b.SetInsertPoint (on_heap);
	m.b.CreateBr (join);
	m.b.SetInsertPoint (join);

	PHINode *address = m.b.CreatePHI (PointerType::get (*m.context, 0), 2);

	address->addIncoming (m.local, on_stack);
	address->addIncoming (m.elsewhere (), on_heap);
	m.store_through (address);

	EXPECT_FALSE (m.fold ());
	EXPECT_EQ (m.barriers (), 1u);
}

} // namespace
} // namespace test
} // namespace mono
