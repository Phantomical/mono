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

// A row that took the scalar overload would still leave its managed body to
// be translated, because the emitter declines non-vector operands. Only the
// row each overload selects tells the two apart.
TEST_F (SimdBodies, OnlyTheVectorOverloadSelectsTheRow)
{
	MonoMethod *vectors = find_method ("Mono.Simd", vector_multiply);
	MonoMethod *scalar = find_method ("Mono.Simd", scalar_multiply);

	ASSERT_NE (vectors, nullptr);
	ASSERT_NE (scalar, nullptr);

	const BuiltinBody *row = builtin_body_for (vectors);

	ASSERT_NE (row, nullptr);
	EXPECT_EQ (row->params, "VV");
	EXPECT_EQ (builtin_body_for (scalar), nullptr);
}

// The scalar overload shares the row's name and arity, and its managed body
// multiplies each lane by one float.
TEST_F (SimdBodies, ScalarMultiplyTranslatesTheManagedBody)
{
	const Translation &multiplied = translate ("Mono.Simd", scalar_multiply);

	ASSERT_TRUE (multiplied.error.empty ()) << multiplied.error;
	ASSERT_NE (multiplied.function, nullptr);

	Shape shape = shape_of (*multiplied.function);

	EXPECT_EQ (shape.vector_fmuls, 0u) << multiplied.text ();
	EXPECT_EQ (shape.scalar_fmuls, 4u) << multiplied.text ();
	EXPECT_GT (shape.instructions, 2u) << multiplied.text ();
}

// The IL adds the same lanes, so tier 0 can keep running it.
TEST_F (SimdBodies, VectorAddStillRunsItsOwnIl)
{
	MonoMethod *method = find_method ("Mono.Simd", vector_add);

	ASSERT_NE (method, nullptr);
	EXPECT_FALSE (builtin_body_replaces_il (method));
}

} // namespace
} // namespace test
} // namespace mono
