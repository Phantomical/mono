/*
 * Tests for mono::arch::plan_interp_entry () and for the key one plan is
 * cached under.
 *
 * The convention being restated is LLVM's own, so what these assert is where
 * LLVM would have put each argument - which register file, which register,
 * which stack slot - for prototypes shaped the way the translator emits them.
 * The corpus running at tier 0 is what proves the two agree; this is what
 * says which answer was meant, and what fails loudly if an LLVM upgrade moves
 * one.
 *
 * No runtime and no metadata: the signatures are built here, and only the four
 * fields the planner reads are filled in.
 */

#include "config.h"

#include "arch/arch.hpp"

#include "mono/metadata/metadata-internals.h"

// This breaks some LLVM headers
#undef PIC

#include <gtest/gtest.h>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include "runtime/interp.hpp"

#include <vector>

using namespace llvm;
using mono::arch::ArgPiece;
using mono::arch::ArgPlan;
using mono::arch::InterpEntryLayout;
using mono::arch::ReturnPlan;

namespace {

/* What the backend compiles against, so struct offsets come out the same. */
constexpr const char *amd64_layout =
	"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128";

/// A signature the planner can read, with a MonoType per parameter so that
/// byref is expressible.
class Signature {
public:
	/// A signature over `byrefs.size ()` parameters, byref where it says so.
	Signature (bool hasthis, std::vector<bool> byrefs)
	{
		types_.resize (byrefs.size ());
		storage_.resize (MONO_SIZEOF_METHOD_SIGNATURE
		                 + byrefs.size () * sizeof (MonoType *));

		MonoMethodSignature *sig = get ();

		sig->param_count = (guint16) byrefs.size ();
		sig->hasthis = hasthis;
		for (size_t i = 0; i < byrefs.size (); ++i) {
			types_[i].byref = byrefs[i];
			sig->params[i] = &types_[i];
		}
	}

	MonoMethodSignature *get ()
	{
		return reinterpret_cast<MonoMethodSignature *> (storage_.data ());
	}

private:
	std::vector<uint8_t> storage_;
	std::vector<MonoType> types_;
};

/// A module holding one declaration, which is all the planner reads.
class Prototype {
public:
	Prototype (Type *ret, ArrayRef<Type *> params)
		: module_ ("interp-entry-tests", context_)
	{
		module_.setDataLayout (amd64_layout);
		function_ = Function::Create (FunctionType::get (ret, params, false),
		                              GlobalValue::ExternalLinkage, "shape",
		                              module_);
	}

	/// Mark parameter `at` as the hidden return pointer to `pointee`.
	void sret (unsigned at, Type *pointee)
	{
		function_->addParamAttr (at, Attribute::getWithStructRetType (context_,
		                                                              pointee));
	}

	Function *get () { return function_; }

private:
	LLVMContext context_;
	Module module_;
	Function *function_ = nullptr;
};

/// The plan, or a gtest failure naming the refusal.
InterpEntryLayout
plan (Prototype &shape, Signature &sig)
{
	Expected<InterpEntryLayout> planned =
		mono::arch::plan_interp_entry (shape.get (), sig.get ());

	if (!planned) {
		ADD_FAILURE () << toString (planned.takeError ());
		return {};
	}

	return std::move (*planned);
}

/// Whether planning refused, consuming the refusal either way.
bool
refused (Prototype &shape, Signature &sig)
{
	Expected<InterpEntryLayout> planned =
		mono::arch::plan_interp_entry (shape.get (), sig.get ());

	if (planned)
		return false;

	consumeError (planned.takeError ());
	return true;
}

void
expect_at (const ArgPlan &arg, ArgPlan::Where where, uint32_t at)
{
	EXPECT_EQ ((int) arg.where, (int) where);
	EXPECT_EQ (arg.at, at);
}

TEST (InterpEntry, ScalarsTakeOneRegisterEach)
{
	LLVMContext ctx;
	Type *i32 = Type::getInt32Ty (ctx);
	Type *f64 = Type::getDoubleTy (ctx);
	Prototype shape (Type::getVoidTy (ctx), { i32, f64, i32, f64 });
	Signature sig (false, { false, false, false, false });
	InterpEntryLayout layout = plan (shape, sig);

	ASSERT_EQ (layout.args.size (), 4u);
	EXPECT_FALSE (layout.has_this);

#ifdef HOST_WIN32
	/* One counter runs both files, so each argument spends the register of
	 * its number in the file it does not want as well. */
	expect_at (layout.args[0], ArgPlan::Where::Greg, 0);
	expect_at (layout.args[1], ArgPlan::Where::Freg, 1);
	expect_at (layout.args[2], ArgPlan::Where::Greg, 2);
	expect_at (layout.args[3], ArgPlan::Where::Freg, 3);
#else
	/* The two files run down independently, so neither pushes the other along. */
	expect_at (layout.args[0], ArgPlan::Where::Greg, 0);
	expect_at (layout.args[1], ArgPlan::Where::Freg, 0);
	expect_at (layout.args[2], ArgPlan::Where::Greg, 1);
	expect_at (layout.args[3], ArgPlan::Where::Freg, 1);
#endif
	EXPECT_EQ ((int) layout.ret.kind, (int) ReturnPlan::Kind::None);
}

TEST (InterpEntry, ReceiverLeadsAndArgumentsFollow)
{
	LLVMContext ctx;
	Type *ptr = PointerType::get (ctx, 0);
	Prototype shape (Type::getInt32Ty (ctx), { ptr, Type::getInt32Ty (ctx) });
	Signature sig (true, { false });
	InterpEntryLayout layout = plan (shape, sig);

	ASSERT_EQ (layout.args.size (), 1u);
	EXPECT_TRUE (layout.has_this);
	EXPECT_EQ (layout.this_greg, 0u);
	expect_at (layout.args[0], ArgPlan::Where::Greg, 1);
}

TEST (InterpEntry, HiddenReturnPointerSitsBehindTheReceiver)
{
	LLVMContext ctx;
	Type *ptr = PointerType::get (ctx, 0);
	Type *i64 = Type::getInt64Ty (ctx);
	StructType *big = StructType::get (ctx, { i64, i64, i64, i64 });
	Prototype shape (Type::getVoidTy (ctx), { ptr, ptr, i64 });

	shape.sret (1, big);

	Signature sig (true, { false });
	InterpEntryLayout layout = plan (shape, sig);

	EXPECT_TRUE (layout.has_this);
	EXPECT_EQ (layout.this_greg, 0u);
	EXPECT_EQ ((int) layout.ret.kind, (int) ReturnPlan::Kind::Hidden);
	EXPECT_EQ (layout.ret.hidden_greg, 1u);
	ASSERT_EQ (layout.args.size (), 1u);
	expect_at (layout.args[0], ArgPlan::Where::Greg, 2);
}

TEST (InterpEntry, AStructIsFlattenedOneFieldPerRegister)
{
	LLVMContext ctx;
	Type *i32 = Type::getInt32Ty (ctx);
	Type *f32 = Type::getFloatTy (ctx);
	StructType *mixed = StructType::get (ctx, { i32, f32 });
	Prototype shape (Type::getVoidTy (ctx), { mixed });
	Signature sig (false, { false });
	InterpEntryLayout layout = plan (shape, sig);

	ASSERT_EQ (layout.args.size (), 1u);
	ASSERT_EQ ((int) layout.args[0].where, (int) ArgPlan::Where::Pieces);
	ASSERT_EQ (layout.args[0].piece_count, 2u);

	const ArgPiece &first = layout.pieces[layout.args[0].first_piece];
	const ArgPiece &second = layout.pieces[layout.args[0].first_piece + 1];

	EXPECT_EQ ((int) first.file, (int) ArgPiece::File::Greg);
	EXPECT_EQ (first.at, 0u);
	EXPECT_EQ (first.offset, 0u);
	EXPECT_EQ ((int) second.file, (int) ArgPiece::File::Freg);
#ifdef HOST_WIN32
	EXPECT_EQ (second.at, 1u);
#else
	EXPECT_EQ (second.at, 0u);
#endif
	EXPECT_EQ (second.offset, 4u);
}

TEST (InterpEntry, PaddingTakesARegisterOfItsOwn)
{
	LLVMContext ctx;
	Type *i8 = Type::getInt8Ty (ctx);
	Type *i64 = Type::getInt64Ty (ctx);
	/*
	 * What the translator spells a hole as: a struct of filler bytes. The
	 * ArrayRef is spelled out because a single element would otherwise reach
	 * StructType::get's isPacked overload and quietly give an empty struct.
	 */
	Type *filler_elements[] = { ArrayType::get (i8, 7) };
	StructType *filler = StructType::get (ctx, ArrayRef<Type *> (filler_elements));
	StructType *holed = StructType::create (ctx, { i8, filler, i64 }, "holed", true);
	Prototype shape (Type::getVoidTy (ctx), { holed });
	Signature sig (false, { false });
	InterpEntryLayout layout = plan (shape, sig);

	ASSERT_EQ (layout.args.size (), 1u);
	EXPECT_EQ (layout.args[0].piece_count, 9u);
	EXPECT_EQ (layout.args[0].size, 16u);
}

TEST (InterpEntry, AStructCanStraddleTheRegisterBoundary)
{
	LLVMContext ctx;
	Type *i32 = Type::getInt32Ty (ctx);
	Type *i64 = Type::getInt64Ty (ctx);
	StructType *pair = StructType::get (ctx, { i32, i32 });

	/* Enough integers ahead of the pair to leave one register free, so the
	 * pair's two leaves land either side of the boundary. The Microsoft
	 * convention has four such registers where System V has six, and its
	 * first stack argument sits past the caller's shadow space. */
#ifdef HOST_WIN32
	std::vector<Type *> params (3, i64);
	const unsigned last_greg = 3, first_stack = 32;
#else
	std::vector<Type *> params (5, i64);
	const unsigned last_greg = 5, first_stack = 0;
#endif
	params.push_back (pair);

	Prototype shape (Type::getVoidTy (ctx), params);
	Signature sig (false, std::vector<bool> (params.size (), false));
	InterpEntryLayout layout = plan (shape, sig);

	const unsigned pair_at = (unsigned) params.size () - 1;

	ASSERT_EQ (layout.args.size (), params.size ());
	ASSERT_EQ ((int) layout.args[pair_at].where, (int) ArgPlan::Where::Pieces);
	ASSERT_EQ (layout.args[pair_at].piece_count, 2u);

	const ArgPiece &first = layout.pieces[layout.args[pair_at].first_piece];
	const ArgPiece &second = layout.pieces[layout.args[pair_at].first_piece + 1];

	/* The last integer register, and then the first stack slot. */
	EXPECT_EQ ((int) first.file, (int) ArgPiece::File::Greg);
	EXPECT_EQ (first.at, last_greg);
	EXPECT_EQ ((int) second.file, (int) ArgPiece::File::Stack);
	EXPECT_EQ (second.at, first_stack);
}

TEST (InterpEntry, StackSlotsAreEightBytesAndSixteenForAVector)
{
	LLVMContext ctx;
	Type *f64 = Type::getDoubleTy (ctx);
	Type *v128 = FixedVectorType::get (Type::getFloatTy (ctx), 4);

#ifdef HOST_WIN32
	/*
	 * The Microsoft convention passes a vector by reference, which the entry
	 * refuses, so the eight-byte slot is all this can check there. Its stack
	 * arguments also start past the caller's shadow space.
	 */
	std::vector<Type *> params (6, f64);
	Prototype shape (Type::getVoidTy (ctx), params);
	Signature sig (false, std::vector<bool> (params.size (), false));
	InterpEntryLayout layout = plan (shape, sig);

	ASSERT_EQ (layout.args.size (), 6u);
	expect_at (layout.args[3], ArgPlan::Where::Freg, 3);
	expect_at (layout.args[4], ArgPlan::Where::Stack, 32);
	expect_at (layout.args[5], ArgPlan::Where::Stack, 40);

	Prototype vector_shape (Type::getVoidTy (ctx), { v128 });
	Signature vector_sig (false, { false });
	EXPECT_TRUE (refused (vector_shape, vector_sig));
#else
	std::vector<Type *> params (8, f64);

	params.push_back (f64);
	params.push_back (v128);

	Prototype shape (Type::getVoidTy (ctx), params);
	Signature sig (false, std::vector<bool> (params.size (), false));
	InterpEntryLayout layout = plan (shape, sig);

	ASSERT_EQ (layout.args.size (), 10u);
	expect_at (layout.args[7], ArgPlan::Where::Freg, 7);
	expect_at (layout.args[8], ArgPlan::Where::Stack, 0);
	expect_at (layout.args[9], ArgPlan::Where::Stack, 16);
#endif
}

TEST (InterpEntry, AByrefParameterIsMarked)
{
	LLVMContext ctx;
	Type *ptr = PointerType::get (ctx, 0);
	Prototype shape (Type::getVoidTy (ctx), { ptr, ptr });
	Signature sig (false, { true, false });
	InterpEntryLayout layout = plan (shape, sig);

	ASSERT_EQ (layout.args.size (), 2u);
	EXPECT_TRUE (layout.args[0].byref);
	EXPECT_FALSE (layout.args[1].byref);
}

TEST (InterpEntry, ARegisterReturnIsScatteredAcrossBothFiles)
{
	LLVMContext ctx;
	StructType *mixed = StructType::get (ctx, { Type::getInt32Ty (ctx),
	                                            Type::getFloatTy (ctx) });
	Prototype shape (mixed, {});
	Signature sig (false, {});
	InterpEntryLayout layout = plan (shape, sig);

	ASSERT_EQ ((int) layout.ret.kind, (int) ReturnPlan::Kind::Registers);
	ASSERT_EQ (layout.ret.pieces.size (), 2u);
	EXPECT_EQ ((int) layout.ret.pieces[0].file, (int) ArgPiece::File::Greg);
	EXPECT_EQ (layout.ret.pieces[0].at, 0u);
	EXPECT_EQ ((int) layout.ret.pieces[1].file, (int) ArgPiece::File::Freg);
	EXPECT_EQ (layout.ret.pieces[1].at, 0u);
}

TEST (InterpEntry, AThirdScalarFloatHasNoReturnRegister)
{
	LLVMContext ctx;
	Type *f32 = Type::getFloatTy (ctx);
	StructType *three = StructType::get (ctx, { f32, f32, f32 });
	Prototype shape (three, {});
	Signature sig (false, {});

	/* A scalar float comes back in xmm0 or xmm1 and nowhere else. */
	EXPECT_TRUE (refused (shape, sig));
}

TEST (InterpEntry, WideVectorsAreRefused)
{
	LLVMContext ctx;
	Type *v256 = FixedVectorType::get (Type::getFloatTy (ctx), 8);
	Prototype shape (Type::getVoidTy (ctx), { v256 });
	Signature sig (false, { false });

	EXPECT_TRUE (refused (shape, sig));
}

TEST (InterpEntry, VarargSignaturesAreRefused)
{
	LLVMContext ctx;
	Prototype shape (Type::getVoidTy (ctx), { Type::getInt32Ty (ctx) });
	Signature sig (false, { false });

	sig.get ()->call_convention = MONO_CALL_VARARG;
	EXPECT_TRUE (refused (shape, sig));
}

/// A ReadOnlySpan as mscorlib lays it out: a reference and a length.
StructType *
span_of_two (LLVMContext &ctx)
{
	StructType *type = StructType::create (ctx, "System.ReadOnlySpan`1<char>");

	type->setBody ({ PointerType::get (ctx, 0), Type::getInt32Ty (ctx),
	                 Type::getInt32Ty (ctx) },
	               /*isPacked=*/true);
	return type;
}

/// A ReadOnlySpan as System.Memory lays it out: an object, a byte offset and a
/// length.
StructType *
span_of_three (LLVMContext &ctx)
{
	StructType *type = StructType::create (ctx, "System.ReadOnlySpan`1<char>");

	type->setBody ({ PointerType::get (ctx, 0), PointerType::get (ctx, 0),
	                 Type::getInt32Ty (ctx), Type::getInt32Ty (ctx) },
	               /*isPacked=*/true);
	return type;
}

TEST (InterpEntryKey, OneStructNameCanStandForTwoLayouts)
{
	/*
	 * Two contexts, because a name is unique inside one: what the runtime has is
	 * two assemblies each defining System.ReadOnlySpan`1, and a layout planned in
	 * a context of its own for each method that arrives.
	 */
	LLVMContext two, three;
	Prototype narrow (PointerType::get (two, 0), { span_of_two (two) });
	Prototype wide (PointerType::get (three, 0), { span_of_three (three) });

	/*
	 * The two arrive in a different number of registers, so sharing a layout
	 * reads the wide one's byte offset four bytes short and picks up its length
	 * in the top half of the pointer.
	 */
	EXPECT_NE (mono::prototype_key (narrow.get ()),
	           mono::prototype_key (wide.get ()));
}

TEST (InterpEntryKey, TwoNamesForOneLayoutShareAKey)
{
	LLVMContext here, there;
	StructType *mine = StructType::create (here, "Some.Pair");
	StructType *yours = StructType::create (there, "Other.Pair");

	mine->setBody ({ PointerType::get (here, 0), Type::getInt32Ty (here) },
	               /*isPacked=*/true);
	yours->setBody ({ PointerType::get (there, 0), Type::getInt32Ty (there) },
	                /*isPacked=*/true);

	Prototype one (Type::getVoidTy (here), { mine });
	Prototype other (Type::getVoidTy (there), { yours });

	EXPECT_EQ (mono::prototype_key (one.get ()),
	           mono::prototype_key (other.get ()));
}

} // namespace
