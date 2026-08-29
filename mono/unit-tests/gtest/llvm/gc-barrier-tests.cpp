/*
 * Tests for the lowerings that write a write barrier back as the card the
 * collector reads, and for the two folds in front of them:
 * `fold_stack_barriers ()` takes a barrier off a store into the frame, and
 * `open_value_copies ()` takes a value copy apart where the IR says an open copy
 * is safe.
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
#include <llvm/IR/IntrinsicInst.h>
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

TEST (GcBarrierTest, AStoreIntoTheFrameMarksNothing)
{
	BarrierModule m (value_decides_layout ());
	IRBuilder<> b (m.site);
	AllocaInst *local = b.CreateAlloca (b.getInt8Ty (), b.getInt32 (32));

	m.site->setArgOperand (0, b.CreateConstInBoundsGEP1_32 (b.getInt8Ty (), local, 8));
	m.lower ();

	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
	EXPECT_EQ (m.module->getFunction (gc_barrier_name), nullptr);
	EXPECT_EQ (m.count ("store i8 1"), 0u) << m.text ();
	EXPECT_EQ (m.count ("@mono_gc_card_table"), 0u) << m.text ();
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

/// A module holding one value copy of a type that holds references.
///
/// \p dest_in_frame and \p src_in_frame each replace a parameter with a local,
/// which is what the fold reads.
struct ValueCopyModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	CallInst *site = nullptr;

	static constexpr uint64_t copied_bytes = 24;

	ValueCopyModule (bool dest_in_frame = false, bool src_in_frame = false,
	                 bool no_overlap = false)
	{
		module = std::make_unique<Module> ("value copies", *context);

		Type *ptr = PointerType::get (*context, 0);

		caller = Function::Create (FunctionType::get (Type::getVoidTy (*context),
		                                              { ptr, ptr }, false),
		                           GlobalValue::ExternalLinkage, "caller", module.get ());

		BasicBlock *entry = BasicBlock::Create (*context, "entry", caller);
		IRBuilder<> b (entry);
		Type *block = ArrayType::get (b.getInt8Ty (), copied_bytes);
		Value *dest = dest_in_frame ? cast<Value> (b.CreateAlloca (block))
		                            : cast<Value> (caller->getArg (0));
		Value *src = src_in_frame ? cast<Value> (b.CreateAlloca (block))
		                          : cast<Value> (caller->getArg (1));

		site = b.CreateCall (gc_value_copy_decl (*module, value_decides_layout ()),
		                     { dest, src, b.getInt32 (1), b.getInt64 (copied_bytes),
		                       Constant::getNullValue (ptr) });
		site->addParamAttr (0, Attribute::getWithAlignment (*context, Align (8)));
		site->addParamAttr (1, Attribute::getWithAlignment (*context, Align (8)));

		if (no_overlap)
			site->addFnAttr (Attribute::get (*context, gc_no_overlap_attr));

		b.CreateRetVoid ();
	}

	bool fold ()
	{
		bool changed = open_value_copies (*caller);

		EXPECT_FALSE (verifyModule (*module, &errs ()));
		return changed;
	}

	void lower () { lower_gc_value_copies (*module); }

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

	unsigned copies () const
	{
		unsigned seen = 0;

		for (const Instruction &in : instructions (*caller))
			seen += isa<MemTransferInst> (&in) ? 1 : 0;

		return seen;
	}

	unsigned sites (StringRef name) const
	{
		Function *decl = module->getFunction (name);

		return decl == nullptr ? 0 : unsigned (decl->getNumUses ());
	}
};

// The copy writes through the destination, which is what tells this declaration
// apart from the two that only mark. The source is read, and neither pointer is
// kept.
TEST (GcValueCopyTest, TheDeclarationWritesThroughItsDestination)
{
	ValueCopyModule m;
	Function *decl = m.module->getFunction (gc_value_copy_name);

	ASSERT_NE (decl, nullptr);

	MemoryEffects effects = decl->getMemoryEffects ();

	EXPECT_TRUE (decl->doesNotThrow ());
	EXPECT_TRUE (isModSet (effects.getModRef (IRMemLocation::ArgMem)));
	EXPECT_TRUE (isRefSet (effects.getModRef (IRMemLocation::ArgMem)));
	EXPECT_TRUE (isModSet (effects.getModRef (IRMemLocation::InaccessibleMem)));
	EXPECT_EQ (effects.getModRef (IRMemLocation::Other), ModRefInfo::NoModRef);

	EXPECT_FALSE (decl->getArg (0)->getAttributes ().hasAttribute (Attribute::ReadOnly));
	EXPECT_TRUE (decl->getArg (1)->getAttributes ().hasAttribute (Attribute::ReadOnly));
	EXPECT_TRUE (decl->getArg (0)->getAttributes ().hasAttribute (Attribute::Captures));
	EXPECT_TRUE (decl->getArg (1)->getAttributes ().hasAttribute (Attribute::Captures));
}

// A destination in the heap keeps the whole call. The copy and its cards reach
// the collector together there, which is what an open copy cannot do.
TEST (GcValueCopyTest, ADestinationInTheHeapStaysOneCall)
{
	ValueCopyModule m;

	EXPECT_FALSE (m.fold ());
	EXPECT_EQ (m.sites (gc_value_copy_name), 1u) << m.text ();
	EXPECT_EQ (m.copies (), 0u) << m.text ();

	m.lower ();

	EXPECT_FALSE (verifyModule (*m.module, &errs ()));
	EXPECT_EQ (m.module->getFunction (gc_value_copy_name), nullptr);
	EXPECT_EQ (m.count ("call void @mono_gc_wbarrier_value_copy_internal(ptr %0, ptr %1, "
	                    "i32 1, ptr null)"),
	           1u)
		<< m.text ();
}

// A source in the frame decides nothing. The destination is what owes the card,
// and this one is in the heap.
TEST (GcValueCopyTest, ASourceInTheFrameKeepsTheCall)
{
	ValueCopyModule m (/*dest_in_frame=*/false, /*src_in_frame=*/true);

	EXPECT_FALSE (m.fold ());
	EXPECT_EQ (m.sites (gc_value_copy_name), 1u) << m.text ();
	EXPECT_EQ (m.copies (), 0u) << m.text ();
}

// A site that says the two cannot overlap gets a memcpy. The box is the producer
// of one: it copies into the object it has just allocated.
TEST (GcValueCopyTest, ASiteThatCannotOverlapGetsAMemcpy)
{
	ValueCopyModule m (/*dest_in_frame=*/true, /*src_in_frame=*/false,
	                   /*no_overlap=*/true);

	EXPECT_TRUE (m.fold ());
	EXPECT_EQ (m.count ("call void @llvm.memcpy"), 1u) << m.text ();
	EXPECT_EQ (m.count ("call void @llvm.memmove"), 0u) << m.text ();
	EXPECT_EQ (m.count ("align 8 "), 2u) << m.text ();
}

// A destination in the frame is a root the collector scans at each collection,
// so the copy owes no card at all and the call becomes the copy it was made of.
//
// No managed producer reaches this arm today: the translator writes a local
// destination as a plain copy already. It is here for a later pass that turns an
// allocation into an alloca.
TEST (GcValueCopyTest, ADestinationInTheFrameOwesNoCards)
{
	ValueCopyModule m (/*dest_in_frame=*/true);

	EXPECT_TRUE (m.fold ());
	EXPECT_EQ (m.sites (gc_value_copy_name), 0u) << m.text ();
	EXPECT_EQ (m.copies (), 1u) << m.text ();
	EXPECT_EQ (m.count ("call void @llvm.memmove"), 1u) << m.text ();
	EXPECT_EQ (m.count ("@mono_gc_card_table"), 0u) << m.text ();
}

} // namespace
} // namespace test
} // namespace mono
