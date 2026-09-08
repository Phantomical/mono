/*
 * Tests for returns_by_hidden_pointer (), which decides whether the translator
 * spells a return's pointer out as an `sret` parameter.
 *
 * Two conventions, one function. On Windows it restates
 * MONO_WIN64_VALUE_TYPE_FITS_REG (arch-amd64.c), the rule the classic tier-0
 * compiler applies for the same signature. Every case below is a size that
 * rule does or does not fit in one register.
 *
 * Everywhere else it restates LLVM's own RetCC_X86_64_C, so what these assert
 * is which aggregates that convention brings back in registers. Every case
 * here was read off `llc` at llvmorg-22.1.8 rather than off place_return (),
 * which states the same convention in arch/amd64/ and is what these have to
 * agree with.
 *
 * A shape called returnable that LLVM demotes costs nothing: LLVM invents the
 * pointer itself. A shape called returnable that LLVM brings back on the x87
 * stack is the failure to catch, and no corpus catches it - a compiled caller
 * and a compiled callee agree, and only the interpreter seam loses the value.
 */

#include "hidden-return.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>

#include <gtest/gtest.h>

using namespace llvm;
using mono::returns_by_hidden_pointer;

namespace {

/// A struct over \p count leaves of type \p leaf.
StructType *
leaves (Type *leaf, unsigned count)
{
	SmallVector<Type *, 4> elements (count, leaf);

	return StructType::get (leaf->getContext (), elements);
}

#ifdef HOST_WIN32

TEST (HiddenReturn, ThePowersOfTwoUpToAMachineWordFit)
{
	LLVMContext ctx;
	Type *i8 = Type::getInt8Ty (ctx);

	EXPECT_FALSE (returns_by_hidden_pointer (leaves (i8, 1)));
	EXPECT_FALSE (returns_by_hidden_pointer (leaves (i8, 2)));
	EXPECT_TRUE (returns_by_hidden_pointer (leaves (i8, 3)));
	EXPECT_FALSE (returns_by_hidden_pointer (leaves (i8, 4)));
	EXPECT_TRUE (returns_by_hidden_pointer (leaves (i8, 6)));
	EXPECT_FALSE (returns_by_hidden_pointer (leaves (i8, 8)));
}

TEST (HiddenReturn, NoSizeAboveAMachineWordFits)
{
	LLVMContext ctx;
	Type *i64 = Type::getInt64Ty (ctx);

	// ReadOnlySpan<char>'s shape: a pointer and a length, 12 bytes. Neither
	// leaf alone crosses a machine word, but Windows never splits an
	// aggregate across two return registers the way SysV does.
	EXPECT_TRUE (returns_by_hidden_pointer (
		StructType::get (ctx, { PointerType::getUnqual (ctx), Type::getInt32Ty (ctx) })));
	EXPECT_TRUE (returns_by_hidden_pointer (leaves (i64, 2)));
}

#else

TEST (HiddenReturn, AScalarIsNeverDemoted)
{
	LLVMContext ctx;

	EXPECT_FALSE (returns_by_hidden_pointer (Type::getFloatTy (ctx)));
	EXPECT_FALSE (returns_by_hidden_pointer (Type::getInt64Ty (ctx)));
	EXPECT_FALSE (returns_by_hidden_pointer (Type::getVoidTy (ctx)));
}

TEST (HiddenReturn, TwoScalarFloatsFitAndAThirdDoesNot)
{
	LLVMContext ctx;
	Type *f32 = Type::getFloatTy (ctx);

	EXPECT_FALSE (returns_by_hidden_pointer (leaves (f32, 2)));
	EXPECT_TRUE (returns_by_hidden_pointer (leaves (f32, 3)));

	/* Quad in mono/mini/tier-seam.cs, and Vector4 field for field. */
	EXPECT_TRUE (returns_by_hidden_pointer (leaves (f32, 4)));
}

TEST (HiddenReturn, ADoubleCountsAsAScalarFloat)
{
	LLVMContext ctx;
	Type *f64 = Type::getDoubleTy (ctx);

	EXPECT_FALSE (returns_by_hidden_pointer (leaves (f64, 2)));
	EXPECT_TRUE (returns_by_hidden_pointer (leaves (f64, 3)));
}

TEST (HiddenReturn, FourVectorsFitAndAFifthDoesNot)
{
	LLVMContext ctx;
	Type *v128 = FixedVectorType::get (Type::getFloatTy (ctx), 4);

	EXPECT_FALSE (returns_by_hidden_pointer (leaves (v128, 4)));
	EXPECT_TRUE (returns_by_hidden_pointer (leaves (v128, 5)));
}

TEST (HiddenReturn, AVectorTakesRoomFromAScalarFloat)
{
	LLVMContext ctx;
	Type *f32 = Type::getFloatTy (ctx);
	Type *v128 = FixedVectorType::get (f32, 4);

	/* The vector takes xmm0, so only one of the two floats is left a register. */
	EXPECT_TRUE (returns_by_hidden_pointer (StructType::get (ctx, { v128, f32, f32 })));

	// Two floats ahead of the vectors reach xmm0 and xmm1, and the vectors follow.
	EXPECT_FALSE (
		returns_by_hidden_pointer (StructType::get (ctx, { f32, f32, v128, v128 })));
}

TEST (HiddenReturn, ThreeIntegerLeavesFitAndAFourthDoesNot)
{
	LLVMContext ctx;
	Type *i64 = Type::getInt64Ty (ctx);

	EXPECT_FALSE (returns_by_hidden_pointer (leaves (i64, 3)));
	EXPECT_TRUE (returns_by_hidden_pointer (leaves (i64, 4)));
}

TEST (HiddenReturn, AWideIntegerClaimsAMachineWordEach)
{
	LLVMContext ctx;
	Type *i128 = Type::getIntNTy (ctx, 128);

	EXPECT_FALSE (returns_by_hidden_pointer (leaves (i128, 1)));
	EXPECT_TRUE (returns_by_hidden_pointer (leaves (i128, 2)));
}

TEST (HiddenReturn, AnArrayIsCountedElementByElement)
{
	LLVMContext ctx;
	Type *f32 = Type::getFloatTy (ctx);

	EXPECT_FALSE (returns_by_hidden_pointer (ArrayType::get (f32, 2)));
	EXPECT_TRUE (returns_by_hidden_pointer (ArrayType::get (f32, 4)));
}

TEST (HiddenReturn, AnExtendedPrecisionFloatIsDemoted)
{
	LLVMContext ctx;

	EXPECT_TRUE (returns_by_hidden_pointer (leaves (Type::getX86_FP80Ty (ctx), 1)));
}

#endif /* HOST_WIN32 */

} // namespace
