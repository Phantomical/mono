/*
 * Tests for the bodies method-to-llvm/simd.cpp writes in place of a SIMD
 * type's managed one.
 *
 * These translate a real Mono.Simd method rather than an il/ fixture. The
 * lowering is keyed to the class the loader marked simd_type, and only that
 * assembly has one.
 */

#include "harness.hpp"

#include "cl-opt-override.hpp"
#include "method-to-llvm/intrinsics.hpp"

#include <glib.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/loader.h>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <gtest/gtest.h>

namespace mono {
namespace test {
namespace {

const char *const vector_add = "Mono.Simd.Vector4f:op_Addition";

// Vector4f declares more than one op_Multiply of two parameters, so a name and
// an arity do not name one of them.
const char *const vector_multiply =
	"Mono.Simd.Vector4f:op_Multiply(Mono.Simd.Vector4f,Mono.Simd.Vector4f)";
const char *const scalar_multiply =
	"Mono.Simd.Vector4f:op_Multiply(Mono.Simd.Vector4f,single)";

const char *const narrow_shift =
	"Mono.Simd.Vector8s:op_RightShift(Mono.Simd.Vector8s,int)";
const char *const saturating_add =
	"Mono.Simd.VectorOperations:AddWithSaturation(Mono.Simd.Vector8s,Mono.Simd.Vector8s)";
const char *const compare_less =
	"Mono.Simd.VectorOperations:CompareLessThan(Mono.Simd.Vector4f,Mono.Simd.Vector4f)";
const char *const float_min =
	"Mono.Simd.VectorOperations:Min(Mono.Simd.Vector4f,Mono.Simd.Vector4f)";
const char *const horizontal_add =
	"Mono.Simd.VectorOperations:HorizontalAdd(Mono.Simd.Vector4f,Mono.Simd.Vector4f)";
const char *const equality =
	"Mono.Simd.Vector4f:op_Equality(Mono.Simd.Vector4f,Mono.Simd.Vector4f)";

// Every op_Explicit Vector4f declares takes one Vector4f and differs only in
// what it answers with, so a signature does not pick one out either. Each is
// the same reinterpretation, so the test asserts on whichever one is found.
const char *const reinterpretation = "Mono.Simd.Vector4f:op_Explicit";

/// Instruction counts read off one translated function.
struct Shape {
	unsigned instructions = 0;
	unsigned calls = 0;
	unsigned vector_fadds = 0;
	unsigned scalar_fadds = 0;
	unsigned vector_fmuls = 0;
	unsigned scalar_fmuls = 0;
};

Shape
shape_of (llvm::Function &function)
{
	Shape shape;

	for (llvm::Instruction &instruction : llvm::instructions (function)) {
		++shape.instructions;

		if (llvm::isa<llvm::CallBase> (instruction))
			++shape.calls;

		bool vector = instruction.getType ()->isVectorTy ();

		switch (instruction.getOpcode ()) {
		case llvm::Instruction::FAdd:
			++(vector ? shape.vector_fadds : shape.scalar_fadds);
			break;
		case llvm::Instruction::FMul:
			++(vector ? shape.vector_fmuls : shape.scalar_fmuls);
			break;
		default:
			break;
		}
	}

	return shape;
}

MonoMethod *
find_method (const char *image, const char *name)
{
	MonoMethodDesc *desc = mono_method_desc_new (name, TRUE);
	MonoMethod *method = mono_method_desc_search_in_image (desc, load_image (image));

	mono_method_desc_free (desc);
	return method;
}

class SimdBodies : public TranslatorTest {};

TEST_F (SimdBodies, VectorAddIsOneVectorFadd)
{
	const Translation &added = translate ("Mono.Simd", vector_add);

	ASSERT_TRUE (added.error.empty ()) << added.error;
	ASSERT_NE (added.function, nullptr);

	Shape shape = shape_of (*added.function);

	EXPECT_EQ (shape.vector_fadds, 1u) << added.text ();
	EXPECT_EQ (shape.calls, 0u) << added.text ();
	EXPECT_EQ (shape.instructions, 2u) << added.text ();
	EXPECT_TRUE (added.function->getReturnType ()->isVectorTy ()) << added.text ();
}

TEST_F (SimdBodies, LoweringOffTranslatesTheManagedBody)
{
	BoolOptionOverride off ("mono-simd", false);
	const Translation &added = translate ("Mono.Simd", vector_add);

	ASSERT_TRUE (added.error.empty ()) << added.error;
	ASSERT_NE (added.function, nullptr);

	Shape shape = shape_of (*added.function);

	EXPECT_EQ (shape.vector_fadds, 0u) << added.text ();
	EXPECT_EQ (shape.scalar_fadds, 4u) << added.text ();
	EXPECT_GT (shape.instructions, 2u) << added.text ();
}

TEST_F (SimdBodies, VectorMultiplyIsOneVectorFmul)
{
	const Translation &multiplied = translate ("Mono.Simd", vector_multiply);

	ASSERT_TRUE (multiplied.error.empty ()) << multiplied.error;
	ASSERT_NE (multiplied.function, nullptr);

	Shape shape = shape_of (*multiplied.function);

	EXPECT_EQ (shape.vector_fmuls, 1u) << multiplied.text ();
	EXPECT_EQ (shape.calls, 0u) << multiplied.text ();
	EXPECT_EQ (shape.instructions, 2u) << multiplied.text ();
	EXPECT_TRUE (multiplied.function->getReturnType ()->isVectorTy ())
		<< multiplied.text ();
}

// Both overloads are lowered, each with a body of its own. The row a method
// selects is what tells them apart, since the name and the arity do not.
TEST_F (SimdBodies, EachMultiplyOverloadSelectsItsOwnRow)
{
	MonoMethod *vectors = find_method ("Mono.Simd", vector_multiply);
	MonoMethod *scalar = find_method ("Mono.Simd", scalar_multiply);

	ASSERT_NE (vectors, nullptr);
	ASSERT_NE (scalar, nullptr);

	const BuiltinBody *vector_row = builtin_body_for (vectors);
	const BuiltinBody *scalar_row = builtin_body_for (scalar);

	ASSERT_NE (vector_row, nullptr);
	ASSERT_NE (scalar_row, nullptr);
	EXPECT_EQ (vector_row->params, "VV");
	EXPECT_EQ (scalar_row->params, "VS");
}

TEST_F (SimdBodies, ScalarMultiplySplatsTheScalar)
{
	const Translation &multiplied = translate ("Mono.Simd", scalar_multiply);

	ASSERT_TRUE (multiplied.error.empty ()) << multiplied.error;
	ASSERT_NE (multiplied.function, nullptr);

	Shape shape = shape_of (*multiplied.function);

	EXPECT_EQ (shape.vector_fmuls, 1u) << multiplied.text ();
	EXPECT_EQ (shape.scalar_fmuls, 0u) << multiplied.text ();
	EXPECT_EQ (shape.calls, 0u) << multiplied.text ();
	EXPECT_EQ (multiplied.count ("insertelement"), 1u) << multiplied.text ();
}

// The IL adds the same lanes, so tier 0 can keep running it.
TEST_F (SimdBodies, VectorAddStillRunsItsOwnIl)
{
	MonoMethod *method = find_method ("Mono.Simd", vector_add);

	ASSERT_NE (method, nullptr);
	EXPECT_FALSE (builtin_body_replaces_il (method));
}

// C# shifts a short as an int and casts back, so the count is masked to 31 and
// not to 15. A shift by 17 then keeps sign bits an i16 shift would drop.
TEST_F (SimdBodies, NarrowShiftRunsAtTheWidthTheIlShiftsAt)
{
	const Translation &shifted = translate ("Mono.Simd", narrow_shift);

	ASSERT_TRUE (shifted.error.empty ()) << shifted.error;
	ASSERT_NE (shifted.function, nullptr);

	EXPECT_EQ (shape_of (*shifted.function).calls, 0u) << shifted.text ();
	EXPECT_EQ (shifted.count ("sext <8 x i16>"), 1u) << shifted.text ();
	EXPECT_EQ (shifted.count (", 31"), 1u) << shifted.text ();
	EXPECT_EQ (shifted.count ("ashr <8 x i32>"), 1u) << shifted.text ();
	EXPECT_EQ (shifted.count ("trunc <8 x i32>"), 1u) << shifted.text ();
}

TEST_F (SimdBodies, SaturatingAddIsOneIntrinsic)
{
	const Translation &added = translate ("Mono.Simd", saturating_add);

	ASSERT_TRUE (added.error.empty ()) << added.error;
	ASSERT_NE (added.function, nullptr);

	EXPECT_EQ (shape_of (*added.function).instructions, 2u) << added.text ();
	EXPECT_EQ (added.count ("llvm.sadd.sat.v8i16"), 1u) << added.text ();
}

// The managed body writes the mask through an int pointer into the operands'
// own struct, so a float struct answers with float lanes holding the bits.
TEST_F (SimdBodies, CompareAnswersAMaskInTheOperandsType)
{
	const Translation &compared = translate ("Mono.Simd", compare_less);

	ASSERT_TRUE (compared.error.empty ()) << compared.error;
	ASSERT_NE (compared.function, nullptr);

	EXPECT_EQ (shape_of (*compared.function).calls, 0u) << compared.text ();
	EXPECT_EQ (compared.count ("fcmp olt <4 x float>"), 1u) << compared.text ();
	EXPECT_EQ (compared.count ("sext <4 x i1>"), 1u) << compared.text ();
	EXPECT_EQ (compared.count ("bitcast <4 x i32>"), 1u) << compared.text ();
	EXPECT_TRUE (compared.function->getReturnType ()->getScalarType ()->isFloatTy ())
		<< compared.text ();
}

// Math.Min answers with its second argument for two zeros of opposite sign and
// with its first for a NaN there, which llvm.minnum does not.
TEST_F (SimdBodies, FloatMinFollowsMathMinsNanRule)
{
	const Translation &smallest = translate ("Mono.Simd", float_min);

	ASSERT_TRUE (smallest.error.empty ()) << smallest.error;
	ASSERT_NE (smallest.function, nullptr);

	EXPECT_EQ (shape_of (*smallest.function).calls, 0u) << smallest.text ();
	EXPECT_EQ (smallest.count ("fcmp olt <4 x float>"), 1u) << smallest.text ();
	EXPECT_EQ (smallest.count ("fcmp uno <4 x float>"), 1u) << smallest.text ();
	EXPECT_EQ (smallest.count ("select"), 2u) << smallest.text ();
	EXPECT_EQ (smallest.count ("minnum"), 0u) << smallest.text ();
}

TEST_F (SimdBodies, HorizontalAddPairsNeighbouringLanes)
{
	const Translation &added = translate ("Mono.Simd", horizontal_add);

	ASSERT_TRUE (added.error.empty ()) << added.error;
	ASSERT_NE (added.function, nullptr);

	Shape shape = shape_of (*added.function);

	EXPECT_EQ (shape.vector_fadds, 1u) << added.text ();
	EXPECT_EQ (shape.calls, 0u) << added.text ();
	EXPECT_EQ (added.count ("<i32 0, i32 2, i32 4, i32 6>"), 1u) << added.text ();
	EXPECT_EQ (added.count ("<i32 1, i32 3, i32 5, i32 7>"), 1u) << added.text ();
}

TEST_F (SimdBodies, ExplicitConversionIsOneBitcast)
{
	const Translation &converted = translate ("Mono.Simd", reinterpretation);

	ASSERT_TRUE (converted.error.empty ()) << converted.error;
	ASSERT_NE (converted.function, nullptr);

	EXPECT_EQ (shape_of (*converted.function).instructions, 2u) << converted.text ();
	EXPECT_EQ (converted.count ("bitcast"), 1u) << converted.text ();
	EXPECT_TRUE (converted.function->getReturnType ()->isVectorTy ())
		<< converted.text ();
}

TEST_F (SimdBodies, EqualityReducesTheLaneCompare)
{
	const Translation &compared = translate ("Mono.Simd", equality);

	ASSERT_TRUE (compared.error.empty ()) << compared.error;
	ASSERT_NE (compared.function, nullptr);

	EXPECT_EQ (compared.count ("fcmp oeq <4 x float>"), 1u) << compared.text ();
	EXPECT_EQ (compared.count ("llvm.vector.reduce.and"), 1u) << compared.text ();
	EXPECT_EQ (compared.count ("zext i1"), 1u) << compared.text ();
	EXPECT_TRUE (compared.function->getReturnType ()->isIntegerTy ())
		<< compared.text ();
}

} // namespace
} // namespace test
} // namespace mono
