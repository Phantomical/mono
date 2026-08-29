/*
 * Tests for allocation_escapes (), which says whether an allocation's pointer
 * can be reached from outside the function that makes it.
 *
 * Pure LLVM. Each case builds the allocation and its users itself, so no runtime
 * and no collector stands under the walk.
 */

#include "analysis/escape.hpp"

#include "passes/alloc-func.hpp"
#include "passes/gc-barrier.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
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

/// The offset the cases write a reference field at. The walk reads no layout, so
/// the value is arbitrary.
constexpr unsigned reference_field = 32;

/// A module holding one caller that answers with a pointer, so a case can make
/// the allocation leave through the return as well as through a store.
struct EscapeModule {
	std::unique_ptr<LLVMContext> context = std::make_unique<LLVMContext> ();
	std::unique_ptr<Module> module;
	Function *caller = nullptr;
	Function *allocator = nullptr;
	Function *sink = nullptr;
	IRBuilder<> b;

	EscapeModule () : b (*context)
	{
		module = std::make_unique<Module> ("escapes", *context);

		Type *ptr = PointerType::get (*context, 0);
		Type *word = Type::getInt64Ty (*context);

		allocator = Function::Create (FunctionType::get (ptr, { word, word }, false),
		                              GlobalValue::ExternalLinkage, "allocator",
		                              module.get ());
		sink = Function::Create (FunctionType::get (Type::getVoidTy (*context), { ptr },
		                                            false),
		                         GlobalValue::ExternalLinkage, "sink", module.get ());
		caller = Function::Create (FunctionType::get (ptr, { ptr }, false),
		                           GlobalValue::ExternalLinkage, "caller", module.get ());

		b.SetInsertPoint (BasicBlock::Create (*context, "entry", caller));
	}

	/// An object the caller was handed, whose own reach the walk cannot see.
	Value *elsewhere () { return caller->getArg (0); }

	CallInst *allocate (AllocShape shape = AllocShape::object)
	{
		Function *decl = alloc_func_decl (*module, shape, /*erasable=*/true);
		Type *word = decl->getFunctionType ()->getParamType (1);

		return b.CreateCall (decl, { ConstantPointerNull::get (b.getPtrTy ()),
		                             ConstantInt::get (word, 48), allocator });
	}

	Value *field (Value *object, unsigned offset)
	{
		return b.CreateConstInBoundsGEP1_32 (b.getInt8Ty (), object, offset);
	}

	/// Writes \p value into the field of \p object at \p offset, and asks for the
	/// card a reference store owes.
	void store_reference (Value *object, unsigned offset, Value *value)
	{
		Value *address = field (object, offset);

		b.CreateAlignedStore (value, address, Align (8));
		b.CreateCall (gc_barrier_decl (*module, GcBarrierLayout ()), { address, value });
	}

	/// Closes the function, then answers whether \p alloc escapes.
	///
	/// \p vouched names the allocations this caller claims stay inside and hand
	/// out nothing they hold, which is what the erasing pass claims of the
	/// objects it takes away in one round.
	bool escapes (CallInst &alloc, ArrayRef<CallInst *> vouched = {})
	{
		if (caller->getEntryBlock ().getTerminator () == nullptr)
			b.CreateRet (ConstantPointerNull::get (b.getPtrTy ()));

		EXPECT_FALSE (verifyModule (*module, &errs ()));

		return allocation_escapes (alloc, [&] (CallBase &holder) {
			for (CallInst *kept : vouched)
				if (kept == &holder)
					return true;

			return false;
		});
	}
};

TEST (EscapeTest, AnObjectNothingHandsOutStaysInside)
{
	EscapeModule m;
	CallInst *object = m.allocate ();

	m.b.CreateAlignedStore (ConstantPointerNull::get (m.b.getPtrTy ()), object, Align (8));

	EXPECT_FALSE (m.escapes (*object));
}

// A read is not a way out. Escape and "nothing reads it" are two questions, and
// a caller that erases an allocation owes the second one of its own.
TEST (EscapeTest, AReadOfAFieldIsNotAWayOut)
{
	EscapeModule m;
	CallInst *object = m.allocate ();

	m.b.CreateAlignedLoad (m.b.getPtrTy (), m.field (object, reference_field), Align (8));

	EXPECT_FALSE (m.escapes (*object));
}

TEST (EscapeTest, ACallThatTakesTheObjectHandsItOut)
{
	EscapeModule m;
	CallInst *object = m.allocate ();

	m.b.CreateCall (m.sink, { object });

	EXPECT_TRUE (m.escapes (*object));
}

TEST (EscapeTest, AnAnswerHandsTheObjectOut)
{
	EscapeModule m;
	CallInst *object = m.allocate ();

	m.b.CreateRet (object);

	EXPECT_TRUE (m.escapes (*object));
}

// The destination is a parameter, so what it holds is reachable from outside.
TEST (EscapeTest, AStoreIntoAnObjectFromOutsideHandsItOut)
{
	EscapeModule m;
	CallInst *object = m.allocate ();

	m.store_reference (m.elsewhere (), reference_field, object);

	EXPECT_TRUE (m.escapes (*object));
}

// The store's destination is an allocation the caller vouches for, so the store
// is not a way out either. LLVM's own tracker reads every store of a pointer as
// a capture, which is the answer this walk replaces.
TEST (EscapeTest, AStoreIntoAnObjectThatStaysInsideIsNotAWayOut)
{
	EscapeModule m;
	CallInst *held = m.allocate ();
	CallInst *holder = m.allocate ();

	m.store_reference (holder, reference_field, held);

	EXPECT_FALSE (m.escapes (*held, { holder }));
	EXPECT_FALSE (m.escapes (*holder));
}

// The same store with nothing vouching for the destination. The walk cannot
// tell on its own that the object holding the pointer hands nothing back out,
// so it reads the store as a way out.
TEST (EscapeTest, AStoreIntoAnObjectNothingVouchesForHandsItOut)
{
	EscapeModule m;
	CallInst *held = m.allocate ();
	CallInst *holder = m.allocate ();

	m.store_reference (holder, reference_field, held);

	EXPECT_TRUE (m.escapes (*held));
}

TEST (EscapeTest, AStoreIntoAnObjectThatIsHandedOutHandsBothOut)
{
	EscapeModule m;
	CallInst *held = m.allocate ();
	CallInst *holder = m.allocate ();

	m.store_reference (holder, reference_field, held);
	m.b.CreateCall (m.sink, { holder });

	EXPECT_TRUE (m.escapes (*held));
	EXPECT_TRUE (m.escapes (*holder));
}

// Two objects that hold each other, with the caller vouching for both. Each
// store names a destination the other side of the pair covers, so neither is a
// way out.
TEST (EscapeTest, TwoObjectsThatHoldEachOtherStayInside)
{
	EscapeModule m;
	CallInst *first = m.allocate ();
	CallInst *second = m.allocate ();

	m.store_reference (first, reference_field, second);
	m.store_reference (second, reference_field, first);

	EXPECT_FALSE (m.escapes (*first, { first, second }));
	EXPECT_FALSE (m.escapes (*second, { first, second }));
}

// The same pair with one side handed out. A caller cannot vouch for an object a
// call takes, so the store into it is a way out for the other side as well.
TEST (EscapeTest, ACycleWithAWayOutHandsBothOut)
{
	EscapeModule m;
	CallInst *first = m.allocate ();
	CallInst *second = m.allocate ();

	m.store_reference (first, reference_field, second);
	m.store_reference (second, reference_field, first);
	m.b.CreateCall (m.sink, { second });

	EXPECT_TRUE (m.escapes (*first, { first }));
	EXPECT_TRUE (m.escapes (*second, { first }));
}

// Each barrier declaration carries captures(none) on both pointers, so LLVM's
// own tracker answers this and the walk needs no rule of its own.
TEST (EscapeTest, ABarrierNamingTheObjectIsNotAWayOut)
{
	EscapeModule m;
	CallInst *object = m.allocate ();
	Value *address = m.field (m.elsewhere (), reference_field);

	m.b.CreateCall (gc_barrier_decl (*m.module, GcBarrierLayout ()), { address, object });
	m.b.CreateCall (gc_value_copy_decl (*m.module, GcBarrierLayout ()),
	                { address, object, m.b.getInt32 (1), m.b.getInt64 (24),
	                  Constant::getNullValue (m.b.getPtrTy ()) });

	EXPECT_FALSE (m.escapes (*object));
}

} // namespace
} // namespace test
} // namespace mono
