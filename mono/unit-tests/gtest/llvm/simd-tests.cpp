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
#include "runtime/inline-scope.hpp"
#include "runtime/options.hpp"

#include <glib.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/metadata.h>

#include <llvm/IR/Attributes.h>
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

const char *const paired_shuffle =
	"Mono.Simd.VectorOperations:Shuffle(Mono.Simd.Vector4f,Mono.Simd.Vector4f,"
	"Mono.Simd.ShuffleSel)";
const char *const whole_shuffle =
	"Mono.Simd.VectorOperations:Shuffle(Mono.Simd.Vector4f,Mono.Simd.ShuffleSel)";
const char *const high_shuffle =
	"Mono.Simd.VectorOperations:ShuffleHigh(Mono.Simd.Vector8s,Mono.Simd.ShuffleSel)";
const char *const absolute_differences =
	"Mono.Simd.VectorOperations:SumOfAbsoluteDifferences(Mono.Simd.Vector16b,"
	"Mono.Simd.Vector16sb)";
const char *const non_temporal_prefetch =
	"Mono.Simd.Vector4f:PrefetchNonTemporal(Mono.Simd.Vector4f&)";
const char *const temporal_prefetch =
	"Mono.Simd.Vector4f:PrefetchTemporalAllCacheLevels(Mono.Simd.Vector4f&)";
const char *const acceleration_mode = "Mono.Simd.SimdRuntime:get_AccelMode";
const char *const aligned_load = "Mono.Simd.Vector4f:LoadAligned(Mono.Simd.Vector4f&)";
const char *const truncating_conversion =
	"Mono.Simd.VectorOperations:ConvertToIntTruncated(Mono.Simd.Vector4f)";
const char *const rounding_conversion =
	"Mono.Simd.VectorOperations:ConvertToInt(Mono.Simd.Vector4f)";
const char *const narrowing_conversion =
	"Mono.Simd.VectorOperations:ConvertToFloat(Mono.Simd.Vector2d)";

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

// The pre-pass bounds an inline by the callee's IL size, and this operator is
// over that limit. What lets it inline anyway is that the backend writes the
// body rather than translating the IL the limit measures.
TEST_F (SimdBodies, TheAddOperatorIsOverThePrePassIlLimit)
{
	MonoMethod *method = find_method ("Mono.Simd", vector_add);

	ASSERT_NE (method, nullptr);

	MonoMethodHeader *header = mono_method_get_header (method);

	ASSERT_NE (header, nullptr);

	guint32 size = 0;

	mono_method_header_get_code (header, &size, nullptr);

	EXPECT_GT (size, trivial_inline_il_limit ());
	EXPECT_TRUE (is_builtin (method));
}

// Turning the lowering off puts the operator back on its own IL, which is what
// the limit is there to measure.
TEST_F (SimdBodies, LoweringOffPutsTheAddOperatorBackUnderTheLimit)
{
	BoolOptionOverride off ("mono-simd", false);
	MonoMethod *method = find_method ("Mono.Simd", vector_add);

	ASSERT_NE (method, nullptr);
	EXPECT_FALSE (is_builtin (method));
}

// C# shifts a short as an int and casts back, so the count is masked to 31 and
// not to 15. A shift by 17 then collapses every lane to its sign, which an
// i16 ashr masked to 15 would not do.
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

// Math.Min answers with its second argument for two zeros of opposite sign.
// It also propagates whichever argument is NaN, rather than suppressing it
// the way llvm.minnum does.
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

// The selector is a parameter, so the lane each field names is known only at
// run time and the read is an extractelement at a computed index. The low half
// of the answer comes from the first operand and the high half from the second.
TEST_F (SimdBodies, PairedShuffleReadsBothOperandsAtAComputedLane)
{
	const Translation &shuffled = translate ("Mono.Simd", paired_shuffle);

	ASSERT_TRUE (shuffled.error.empty ()) << shuffled.error;
	ASSERT_NE (shuffled.function, nullptr);

	EXPECT_EQ (shape_of (*shuffled.function).calls, 0u) << shuffled.text ();
	EXPECT_EQ (shuffled.count ("extractelement <4 x float>"), 4u) << shuffled.text ();
	EXPECT_EQ (shuffled.count ("insertelement <4 x float>"), 4u) << shuffled.text ();
	EXPECT_EQ (shuffled.count ("and i32"), 4u) << shuffled.text ();
}

// ShuffleHigh writes the four high lanes and copies the four low ones, so the
// answer it builds on is the operand rather than a poison vector.
TEST_F (SimdBodies, HighShuffleBuildsOnTheOperandItWasGiven)
{
	const Translation &shuffled = translate ("Mono.Simd", high_shuffle);
	const Translation &whole = translate ("Mono.Simd", whole_shuffle);

	ASSERT_TRUE (shuffled.error.empty ()) << shuffled.error;
	ASSERT_NE (shuffled.function, nullptr);
	ASSERT_TRUE (whole.error.empty ()) << whole.error;
	ASSERT_NE (whole.function, nullptr);

	EXPECT_EQ (shuffled.count ("extractelement <8 x i16>"), 4u) << shuffled.text ();
	EXPECT_EQ (shuffled.count ("insertelement <8 x i16>"), 4u) << shuffled.text ();
	EXPECT_EQ (shuffled.count ("add i32"), 4u) << shuffled.text ();
	EXPECT_EQ (shuffled.count ("poison"), 0u) << shuffled.text ();
	EXPECT_EQ (whole.count ("poison"), 1u) << whole.text ();
}

// The managed body reads its first operand through a byte pointer and its
// second through an sbyte one, which is what psadbw does not do.
TEST_F (SimdBodies, AbsoluteDifferencesReadOneOperandSigned)
{
	const Translation &summed = translate ("Mono.Simd", absolute_differences);

	ASSERT_TRUE (summed.error.empty ()) << summed.error;
	ASSERT_NE (summed.function, nullptr);

	EXPECT_EQ (summed.count ("zext <16 x i8>"), 1u) << summed.text ();
	EXPECT_EQ (summed.count ("sext <16 x i8>"), 1u) << summed.text ();
	EXPECT_EQ (summed.count ("llvm.abs.v16i32"), 1u) << summed.text ();
	EXPECT_EQ (summed.count ("llvm.vector.reduce.add.v8i32"), 2u) << summed.text ();
	EXPECT_EQ (summed.count ("insertelement <8 x i16>"), 2u) << summed.text ();
}

// A plain fptosi is poison out of range, which LLVM folds to zero where it can
// see the operand and leaves as the hardware answer where it cannot.
TEST_F (SimdBodies, TruncatingConversionUsesTheConstrainedIntrinsic)
{
	const Translation &converted = translate ("Mono.Simd", truncating_conversion);

	ASSERT_TRUE (converted.error.empty ()) << converted.error;
	ASSERT_NE (converted.function, nullptr);

	EXPECT_EQ (converted.count ("experimental.constrained.fptosi.v4i32.v4f32"), 1u)
		<< converted.text ();
	EXPECT_EQ (converted.count ("roundeven"), 0u) << converted.text ();
	EXPECT_TRUE (converted.function->hasFnAttribute (llvm::Attribute::StrictFP))
		<< converted.text ();
}

// ConvertToInt rounds through System.Math.Round, which takes a double, so a
// float lane widens before it rounds.
TEST_F (SimdBodies, RoundingConversionRoundsAtDoubleWidth)
{
	const Translation &converted = translate ("Mono.Simd", rounding_conversion);

	ASSERT_TRUE (converted.error.empty ()) << converted.error;
	ASSERT_NE (converted.function, nullptr);

	EXPECT_EQ (converted.count ("fpext <4 x float>"), 1u) << converted.text ();
	EXPECT_EQ (converted.count ("llvm.roundeven.v4f64"), 1u) << converted.text ();
	EXPECT_EQ (converted.count ("experimental.constrained.fptosi.v4i32.v4f64"), 1u)
		<< converted.text ();
}

// Vector2d holds two lanes and Vector4f four, so the body writes a zero into
// each lane it has nothing to convert for.
TEST_F (SimdBodies, WideningConversionZeroFillsTheLanesTheBodyDoesNotWrite)
{
	const Translation &converted = translate ("Mono.Simd", narrowing_conversion);

	ASSERT_TRUE (converted.error.empty ()) << converted.error;
	ASSERT_NE (converted.function, nullptr);

	EXPECT_EQ (shape_of (*converted.function).calls, 0u) << converted.text ();
	EXPECT_EQ (converted.count ("fptrunc <2 x double>"), 1u) << converted.text ();
	EXPECT_EQ (converted.count ("zeroinitializer"), 1u) << converted.text ();
}

// The managed body is empty and exists for a compiler to recognize. A prefetch
// reaches no value the program can read, so the row leaves every answer alone.
//
// The trailing 1 is the data cache. Passing 0 there names the instruction cache,
// which x86 lowers to no instruction at all, and no differential test could see
// that because a prefetch has nothing to compare.
TEST_F (SimdBodies, PrefetchAsksTheDataCacheForARead)
{
	const Translation &asked = translate ("Mono.Simd", non_temporal_prefetch);

	ASSERT_TRUE (asked.error.empty ()) << asked.error;
	ASSERT_NE (asked.function, nullptr);

	EXPECT_EQ (asked.count ("llvm.prefetch"), 1u) << asked.text ();
	EXPECT_EQ (asked.count ("i32 0, i32 0, i32 1"), 1u) << asked.text ();
	EXPECT_TRUE (asked.function->getReturnType ()->isVoidTy ()) << asked.text ();
}

// The four names run from the whole hierarchy down to non-temporal, which is
// llvm.prefetch's locality counting the other way: 3 selects prefetcht0 and 0
// selects prefetchnta.
TEST_F (SimdBodies, EachPrefetchNameTakesItsOwnLocality)
{
	const Translation &all_levels = translate ("Mono.Simd", temporal_prefetch);

	ASSERT_TRUE (all_levels.error.empty ()) << all_levels.error;
	ASSERT_NE (all_levels.function, nullptr);

	EXPECT_EQ (all_levels.count ("i32 0, i32 3, i32 1"), 1u) << all_levels.text ();
}

// AccelMode's managed body answers None whatever the target is, so a row could
// only answer by computing something its own IL does not. il_agrees false is
// not enough to make that safe: it reaches runs_at_tier0 (), which decides only
// for methods the backend is asked about, and the interpreter never asks about
// a callee it reached itself. Such a row measured 0x0 under an interpreted
// caller against 0x3F under a compiled one on the same host. The interpreter is
// tier 0 only under -mono-tier0-classic=0, so that is the arm the hole is in.
TEST_F (SimdBodies, AccelModeIsLeftOnItsOwnIl)
{
	MonoMethod *method = find_method ("Mono.Simd", acceleration_mode);

	ASSERT_NE (method, nullptr);
	EXPECT_EQ (builtin_body_for (method), nullptr);
	EXPECT_FALSE (builtin_body_replaces_il (method));
}

// LoadAligned's managed body is a copy through a byref, which the translator
// already writes as one vector load. A row could only add an alignment the
// body does not claim, so there is none.
TEST_F (SimdBodies, AlignedLoadIsLeftOnItsOwnIl)
{
	MonoMethod *method = find_method ("Mono.Simd", aligned_load);

	ASSERT_NE (method, nullptr);
	EXPECT_EQ (builtin_body_for (method), nullptr);
}

} // namespace
} // namespace test
} // namespace mono
