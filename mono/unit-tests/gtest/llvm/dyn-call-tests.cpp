/*
 * Tests for mono::arch::plan_dyn_call (), the interpreter's own reading of the
 * convention interp-entry-tests.cpp checks from the other direction.
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

#include <memory>
#include <vector>

using namespace llvm;
using mono::arch::ArgPiece;
using mono::arch::DynCallPlan;
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
		: module_ ("dyn-call-tests", context_)
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
std::unique_ptr<DynCallPlan>
plan (Prototype &shape, Signature &sig)
{
	Expected<std::unique_ptr<DynCallPlan>> planned =
		mono::arch::plan_dyn_call (shape.get (), sig.get ());

	if (!planned) {
		ADD_FAILURE () << toString (planned.takeError ());
		return nullptr;
	}

	return std::move (*planned);
}

/// Whether planning refused, consuming the refusal either way.
bool
refused (Prototype &shape, Signature &sig)
{
	Expected<std::unique_ptr<DynCallPlan>> planned =
		mono::arch::plan_dyn_call (shape.get (), sig.get ());

	if (planned)
		return false;

	consumeError (planned.takeError ());
	return true;
}

/// A struct of `count` one-byte fields, one leaf per field once flattened -
/// the shape mono/tests/tier0-classic-vret-spill.cs's VretSpillWide takes.
StructType *
bytes (LLVMContext &ctx, unsigned count)
{
	std::vector<Type *> fields (count, Type::getInt8Ty (ctx));

	return StructType::get (ctx, fields, /*isPacked=*/true);
}

TEST (DynCall, HiddenReturnPointerSitsBehindTheReceiver)
{
	LLVMContext ctx;
	Type *ptr = PointerType::get (ctx, 0);
	Type *i64 = Type::getInt64Ty (ctx);
	StructType *big = StructType::get (ctx, { i64, i64, i64, i64 });
	Prototype shape (Type::getVoidTy (ctx), { ptr, ptr, i64 });

	shape.sret (1, big);

	Signature sig (true, { false });
	std::unique_ptr<DynCallPlan> planned = plan (shape, sig);

	ASSERT_NE (planned, nullptr);
	EXPECT_EQ ((int) planned->ret.kind, (int) ReturnPlan::Kind::Hidden);
	EXPECT_EQ ((int) planned->ret.hidden.file, (int) ArgPiece::File::Greg);
	EXPECT_EQ (planned->ret.hidden.at, 1u);
}

// The sixteen one-byte fields spend every integer parameter register, so the
// hidden return pointer behind them lands on the stack instead.
// mono/tests/tier0-classic-vret-spill.cs gates the same shape end to end.
TEST (DynCall, HiddenReturnPointerCanSpillToTheStack)
{
	LLVMContext ctx;
	Type *ptr = PointerType::get (ctx, 0);
	StructType *wide = bytes (ctx, 16);
	Prototype shape (Type::getVoidTy (ctx), { wide, ptr });

	shape.sret (1, wide);

	Signature sig (false, { false });
	std::unique_ptr<DynCallPlan> planned = plan (shape, sig);

	ASSERT_NE (planned, nullptr);
	EXPECT_EQ ((int) planned->ret.kind, (int) ReturnPlan::Kind::Hidden);
	EXPECT_EQ ((int) planned->ret.hidden.file, (int) ArgPiece::File::Stack);

	// Six one-byte leaves fill the integer registers. The other ten each take
	// an eight-byte stack slot ahead of the pointer's own.
	EXPECT_EQ (planned->ret.hidden.at, 10u * 8u);
}

TEST (DynCall, WideVectorsAreRefused)
{
	LLVMContext ctx;
	Type *v256 = FixedVectorType::get (Type::getFloatTy (ctx), 8);
	Prototype shape (Type::getVoidTy (ctx), { v256 });
	Signature sig (false, { false });

	EXPECT_TRUE (refused (shape, sig));
}

TEST (DynCall, AReceiverAheadOfASpilledWideArgumentStillTakesARegister)
{
	LLVMContext ctx;
	Type *ptr = PointerType::get (ctx, 0);
	StructType *wide = bytes (ctx, 16);
	Prototype shape (Type::getVoidTy (ctx), { ptr, ptr, wide });

	shape.sret (1, wide);

	Signature sig (true, { false });
	std::unique_ptr<DynCallPlan> planned = plan (shape, sig);

	ASSERT_NE (planned, nullptr);
	// The receiver leads the prototype, so the pointer behind it is still
	// the second parameter. The wide argument behind that never spends
	// the file.
	EXPECT_EQ ((int) planned->ret.hidden.file, (int) ArgPiece::File::Greg);
	EXPECT_EQ (planned->ret.hidden.at, 1u);
}

} // namespace
