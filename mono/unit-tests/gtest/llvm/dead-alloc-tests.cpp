/*
 * Tests for erase_dead_allocations (), which takes an object nothing reads away
 * with its field stores and the write barriers beside them.
 *
 * Pure LLVM. Each case builds the allocation and the barriers itself, so no
 * runtime and no collector stands under the walk.
 */

#include "passes/alloc-func.hpp"
#include "passes/dead-alloc.hpp"
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

/// The offset the cases write a reference field at. The walk reads no layout,
/// so the value is arbitrary.
constexpr unsigned reference_field = 32;

/// The size of the value type the copy cases move. Arbitrary for the same
/// reason.
constexpr uint64_t copied_bytes = 24;

/// A module holding one caller and the allocator its allocation sites name.
struct AllocModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	Function *allocator = nullptr;
	IRBuilder<> b;

	AllocModule () : b (*context)
	{
		module = std::make_unique<Module> ("dead allocations", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *word = Type::getInt64Ty (*context);

		allocator = Function::Create (FunctionType::get (ptr, { word, word }, false),
		                              GlobalValue::ExternalLinkage, "allocator",
		                              module.get ());
		caller = Function::Create (FunctionType::get (Type::getVoidTy (*context),
		                                              { ptr, ptr }, false),
		                           GlobalValue::ExternalLinkage, "caller", module.get ());

		b.SetInsertPoint (BasicBlock::Create (*context, "entry", caller));
	}

	/// A reference the caller was handed, which a field store writes.
	Value *reference () { return caller->getArg (0); }

	/// An object the caller was handed, which the walk cannot see the stores of.
	Value *elsewhere () { return caller->getArg (1); }

	CallInst *allocate (AllocShape shape = AllocShape::object, bool erasable = true)
	{
		Function *decl = alloc_func_decl (*module, shape, erasable);
		Type *word = decl->getFunctionType ()->getParamType (1);

		return b.CreateCall (decl, { ConstantPointerNull::get (b.getPtrTy ()),
		                             ConstantInt::get (word, 48), allocator });
	}

	Value *field (Value *object, unsigned offset)
	{
		return b.CreateConstInBoundsGEP1_32 (b.getInt8Ty (), object, offset);
	}

	/// Writes \p value into the field of \p object at \p offset, and asks for
	/// the card a reference store owes.
	void store_reference (Value *object, unsigned offset, Value *value)
	{
		Value *address = field (object, offset);

		b.CreateAlignedStore (value, address, Align (8));
		barrier (address, value);
	}

	void barrier (Value *address, Value *value)
	{
		b.CreateCall (gc_barrier_decl (*module, GcBarrierLayout ()), { address, value });
	}

	/// Copies a value type into the field of \p object at \p offset, as the fold
	/// leaves such a copy where the destination is a frame slot. A copy in the
	/// open owes no cards, so no barrier stands beside it.
	void copy_into (Value *object, unsigned offset, Value *source,
	                bool is_volatile = false)
	{
		b.CreateMemCpy (field (object, offset), Align (8), source, Align (8),
		                b.getInt64 (copied_bytes), is_volatile);
	}

	/// The one call a value copy is where the fold leaves it standing, which is
	/// every destination the IR does not settle to a frame slot.
	void value_copy (Value *dest, Value *source)
	{
		b.CreateCall (gc_value_copy_decl (*module, GcBarrierLayout ()),
		              { dest, source, b.getInt32 (1), b.getInt64 (copied_bytes),
		                Constant::getNullValue (b.getPtrTy ()) });
	}

	unsigned value_copies ()
	{
		Function *decl = module->getFunction (gc_value_copy_name);

		return decl == nullptr ? 0 : unsigned (decl->getNumUses ());
	}

	unsigned copies () const
	{
		unsigned seen = 0;

		for (const Instruction &in : instructions (*caller))
			seen += isa<MemTransferInst> (&in) ? 1 : 0;

		return seen;
	}

	/// How many sites the declaration of one form still has.
	unsigned allocations (AllocShape shape = AllocShape::object, bool erasable = true)
	{
		return unsigned (alloc_func_decl (*module, shape, erasable)->getNumUses ());
	}

	unsigned barriers ()
	{
		Function *decl = module->getFunction (gc_barrier_name);

		return decl == nullptr ? 0 : unsigned (decl->getNumUses ());
	}

	unsigned stores () const
	{
		unsigned seen = 0;

		for (const Instruction &in : instructions (*caller))
			seen += isa<StoreInst> (&in) ? 1 : 0;

		return seen;
	}

	bool erase ()
	{
		b.CreateRetVoid ();

		bool changed = erase_dead_allocations (*caller);

		EXPECT_FALSE (verifyModule (*module, &errs ()));
		return changed;
	}

	std::string text () const
	{
		std::string printed;
		raw_string_ostream out (printed);

		module->print (out, nullptr);
		return printed;
	}
};

TEST (DeadAllocTest, AnObjectNothingReadsGoesWithItsStoresAndItsBarriers)
{
	AllocModule m;
	CallInst *object = m.allocate ();

	// The vtable store the translator writes beside every allocation.
	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), object, Align (8));
	m.b.CreateAlignedStore (m.b.getInt32 (7), m.field (object, 16), Align (4));
	m.store_reference (object, reference_field, m.reference ());

	EXPECT_TRUE (m.erase ());
	EXPECT_EQ (m.allocations (), 0u) << m.text ();
	EXPECT_EQ (m.barriers (), 0u) << m.text ();
	EXPECT_EQ (m.stores (), 0u) << m.text ();
}

TEST (DeadAllocTest, AVectorGoesTheSameWay)
{
	AllocModule m;
	CallInst *vector = m.allocate (AllocShape::vector);

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), vector, Align (8));
	m.store_reference (vector, reference_field, m.reference ());

	EXPECT_TRUE (m.erase ());
	EXPECT_EQ (m.allocations (AllocShape::vector), 0u) << m.text ();
	EXPECT_EQ (m.barriers (), 0u) << m.text ();
}

TEST (DeadAllocTest, AReadOfAFieldKeepsTheObject)
{
	AllocModule m;
	CallInst *object = m.allocate ();

	m.store_reference (object, reference_field, m.reference ());
	m.b.CreateAlignedLoad (m.b.getPtrTy (), m.field (object, reference_field), Align (8));

	EXPECT_FALSE (m.erase ());
	EXPECT_EQ (m.allocations (), 1u) << m.text ();
	EXPECT_EQ (m.barriers (), 1u) << m.text ();
	EXPECT_EQ (m.stores (), 1u) << m.text ();
}

TEST (DeadAllocTest, AnObjectStoredIntoAnotherObjectStays)
{
	AllocModule m;
	CallInst *object = m.allocate ();

	m.store_reference (object, reference_field, m.reference ());
	m.store_reference (m.elsewhere (), reference_field, object);

	EXPECT_FALSE (m.erase ());
	EXPECT_EQ (m.allocations (), 1u) << m.text ();
	EXPECT_EQ (m.barriers (), 2u) << m.text ();
}

// The store that captures the object writes through a pointer the walk never
// reaches, so the barrier is all the walk finds.
TEST (DeadAllocTest, ABarrierThatNamesTheObjectAsTheValueStays)
{
	AllocModule m;
	CallInst *object = m.allocate ();

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), object, Align (8));
	m.barrier (m.field (m.elsewhere (), reference_field), object);

	EXPECT_FALSE (m.erase ());
	EXPECT_EQ (m.allocations (), 1u) << m.text ();
	EXPECT_EQ (m.barriers (), 1u) << m.text ();
}

// The `.kept` name says the program can tell the allocation happened.
TEST (DeadAllocTest, AKeptAllocationIsLeftAlone)
{
	AllocModule m;
	CallInst *object = m.allocate (AllocShape::object, false);

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), object, Align (8));
	m.store_reference (object, reference_field, m.reference ());

	EXPECT_FALSE (m.erase ());
	EXPECT_EQ (m.allocations (AllocShape::object, false), 1u) << m.text ();
	EXPECT_EQ (m.barriers (), 1u) << m.text ();
	EXPECT_EQ (m.stores (), 2u) << m.text ();
}

TEST (DeadAllocTest, AnObjectDiesWithTheObjectThatHeldIt)
{
	AllocModule m;
	CallInst *held = m.allocate ();
	CallInst *holder = m.allocate ();

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), held, Align (8));
	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), holder, Align (8));
	m.store_reference (holder, reference_field, held);

	EXPECT_TRUE (m.erase ());
	EXPECT_EQ (m.allocations (), 0u) << m.text ();
	EXPECT_EQ (m.barriers (), 0u) << m.text ();
}

// A copy the fold opened writes into the object, which is the same user as a
// store.
TEST (DeadAllocTest, AnObjectAnOpenCopyFilledGoesWithTheCopy)
{
	AllocModule m;
	CallInst *object = m.allocate ();

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), object, Align (8));
	m.copy_into (object, reference_field, m.elsewhere ());

	EXPECT_TRUE (m.erase ());
	EXPECT_EQ (m.allocations (), 0u) << m.text ();
	EXPECT_EQ (m.copies (), 0u) << m.text ();
}

// The object is the source, so the copy reads it out into memory the walk never
// reaches.
TEST (DeadAllocTest, ACopyThatReadsTheObjectKeepsIt)
{
	AllocModule m;
	CallInst *object = m.allocate ();

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), object, Align (8));
	m.b.CreateMemCpy (m.elsewhere (), Align (8), object, Align (8),
	                  m.b.getInt64 (copied_bytes));

	EXPECT_FALSE (m.erase ());
	EXPECT_EQ (m.allocations (), 1u) << m.text ();
	EXPECT_EQ (m.copies (), 1u) << m.text ();
}

// A value copy that names the object as its source reads it out into memory the
// walk never reaches. Taking that call for one of our own would erase an object
// something else now holds a copy of.
TEST (DeadAllocTest, AValueCopyThatNamesTheObjectAsTheSourceStays)
{
	AllocModule m;
	CallInst *object = m.allocate ();

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), object, Align (8));
	m.value_copy (m.field (m.elsewhere (), reference_field), object);

	EXPECT_FALSE (m.erase ());
	EXPECT_EQ (m.allocations (), 1u) << m.text ();
	EXPECT_EQ (m.value_copies (), 1u) << m.text ();
}

// A volatile copy is an event of its own, the same as a volatile store.
TEST (DeadAllocTest, AVolatileCopyKeepsTheObject)
{
	AllocModule m;
	CallInst *object = m.allocate ();

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), object, Align (8));
	m.copy_into (object, reference_field, m.elsewhere (), /*is_volatile=*/true);

	EXPECT_FALSE (m.erase ());
	EXPECT_EQ (m.allocations (), 1u) << m.text ();
	EXPECT_EQ (m.copies (), 1u) << m.text ();
}

// A value copy the fold left standing writes into the object, which is the same
// user as a store with its barrier.
TEST (DeadAllocTest, AnObjectAValueCopyWroteIntoGoesWithTheCopy)
{
	AllocModule m;
	CallInst *object = m.allocate ();

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), object, Align (8));
	m.value_copy (m.field (object, reference_field), m.elsewhere ());

	EXPECT_TRUE (m.erase ());
	EXPECT_EQ (m.allocations (), 0u) << m.text ();
	EXPECT_EQ (m.value_copies (), 0u) << m.text ();
}

// The copy reads the object out into memory the walk never reaches.
TEST (DeadAllocTest, AValueCopyThatReadsTheObjectKeepsIt)
{
	AllocModule m;
	CallInst *object = m.allocate ();

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), object, Align (8));
	m.value_copy (m.field (m.elsewhere (), reference_field), object);

	EXPECT_FALSE (m.erase ());
	EXPECT_EQ (m.allocations (), 1u) << m.text ();
	EXPECT_EQ (m.value_copies (), 1u) << m.text ();
}

// Two objects that hold each other and that nothing else reads. Each one is dead
// only because the other is, which is the shape a walk over one object at a time
// cannot settle.
TEST (DeadAllocTest, TwoObjectsThatHoldEachOtherGoTogether)
{
	AllocModule m;
	CallInst *first = m.allocate ();
	CallInst *second = m.allocate ();

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), first, Align (8));
	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), second, Align (8));
	m.store_reference (first, reference_field, second);
	m.store_reference (second, reference_field, first);

	EXPECT_TRUE (m.erase ());
	EXPECT_EQ (m.allocations (), 0u) << m.text ();
	EXPECT_EQ (m.barriers (), 0u) << m.text ();
	EXPECT_EQ (m.stores (), 0u) << m.text ();
}

// The same pair with a reader on one arm. A read of either object keeps both,
// because the one that is read still holds the other.
TEST (DeadAllocTest, ACycleWithAReaderKeepsBothObjects)
{
	AllocModule m;
	CallInst *first = m.allocate ();
	CallInst *second = m.allocate ();

	m.store_reference (first, reference_field, second);
	m.store_reference (second, reference_field, first);
	m.b.CreateAlignedLoad (m.b.getPtrTy (), m.field (first, reference_field), Align (8));

	EXPECT_FALSE (m.erase ());
	EXPECT_EQ (m.allocations (), 2u) << m.text ();
	EXPECT_EQ (m.barriers (), 2u) << m.text ();
}

// A call reaches the object from outside the function, whatever it does with it.
TEST (DeadAllocTest, ACallThatTakesTheObjectKeepsIt)
{
	AllocModule m;
	CallInst *object = m.allocate ();

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), object, Align (8));
	m.b.CreateCall (m.caller, { object, m.reference () });

	EXPECT_FALSE (m.erase ());
	EXPECT_EQ (m.allocations (), 1u) << m.text ();
}

} // namespace
} // namespace test
} // namespace mono
