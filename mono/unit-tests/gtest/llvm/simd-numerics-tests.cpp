/*
 * Tests for the bodies method-to-llvm/simd-numerics.cpp writes in place of
 * System.Numerics.Vector4's managed ones.
 *
 * These translate a real Vector4 method rather than an il/ fixture, for the
 * reason simd-tests.cpp gives.
 */

#include "harness.hpp"

#include "cl-opt-override.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <gtest/gtest.h>

namespace mono {
namespace test {
namespace {

const char *const numerics = "System.Numerics";

const char *const vector_add = "System.Numerics.Vector4:op_Addition";
const char *const vector_min = "System.Numerics.Vector4:Min";
const char *const vector_dot = "System.Numerics.Vector4:Dot";

// Vector4 declares three op_Multiply of two parameters, so a name and an arity
// do not name one of them.
const char *const scalar_multiply =
	"System.Numerics.Vector4:op_Multiply(System.Numerics.Vector4,single)";

/// Instruction counts read off one translated function.
struct Shape {
	unsigned instructions = 0;
	unsigned calls = 0;
	unsigned vector_fadds = 0;
	unsigned scalar_fadds = 0;
	unsigned vector_fmuls = 0;
	unsigned scalar_fmuls = 0;
	unsigned selects = 0;
	unsigned shuffles = 0;
	unsigned ordered_less = 0;
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
		case llvm::Instruction::Select:
			++shape.selects;
			break;
		case llvm::Instruction::ShuffleVector:
			++shape.shuffles;
			break;
		case llvm::Instruction::FCmp:
			if (llvm::cast<llvm::FCmpInst> (instruction).getPredicate ()
			    == llvm::CmpInst::FCMP_OLT)
				++shape.ordered_less;
			break;
		default:
			break;
		}
	}

	return shape;
}

class SimdNumericsBodies : public TranslatorTest {};

TEST_F (SimdNumericsBodies, VectorAddIsOneVectorFadd)
{
	const Translation &added = translate (numerics, vector_add);

	ASSERT_TRUE (added.error.empty ()) << added.error;
	ASSERT_NE (added.function, nullptr);

	Shape shape = shape_of (*added.function);

	EXPECT_EQ (shape.vector_fadds, 1u) << added.text ();
	EXPECT_EQ (shape.calls, 0u) << added.text ();
	EXPECT_EQ (shape.instructions, 2u) << added.text ();
	EXPECT_TRUE (added.function->getReturnType ()->isVectorTy ()) << added.text ();
}

TEST_F (SimdNumericsBodies, LoweringOffTranslatesTheManagedBody)
{
	BoolOptionOverride off ("mono-simd", false);
	const Translation &added = translate (numerics, vector_add);

	ASSERT_TRUE (added.error.empty ()) << added.error;
	ASSERT_NE (added.function, nullptr);

	Shape shape = shape_of (*added.function);

	EXPECT_EQ (shape.vector_fadds, 0u) << added.text ();
	EXPECT_EQ (shape.scalar_fadds, 4u) << added.text ();
	EXPECT_GT (shape.instructions, 2u) << added.text ();
}

// A NaN in either operand makes the managed compare false, so the second
// operand wins. llvm.minnum would answer the other one.
TEST_F (SimdNumericsBodies, MinIsACompareAndASelect)
{
	const Translation &least = translate (numerics, vector_min);

	ASSERT_TRUE (least.error.empty ()) << least.error;
	ASSERT_NE (least.function, nullptr);

	Shape shape = shape_of (*least.function);

	EXPECT_EQ (shape.ordered_less, 1u) << least.text ();
	EXPECT_EQ (shape.selects, 1u) << least.text ();
	EXPECT_EQ (shape.calls, 0u) << least.text ();
	EXPECT_EQ (least.count ("minnum"), 0u) << least.text ();
}

// The products are added lowest lane first, the association the managed body
// has. A vector reduce would add its own start value in front of them.
TEST_F (SimdNumericsBodies, DotAddsFourProductsInLaneOrder)
{
	const Translation &product = translate (numerics, vector_dot);

	ASSERT_TRUE (product.error.empty ()) << product.error;
	ASSERT_NE (product.function, nullptr);

	Shape shape = shape_of (*product.function);

	EXPECT_EQ (shape.scalar_fmuls, 4u) << product.text ();
	EXPECT_EQ (shape.scalar_fadds, 3u) << product.text ();
	EXPECT_EQ (shape.vector_fadds, 0u) << product.text ();
	EXPECT_EQ (shape.calls, 0u) << product.text ();
	EXPECT_EQ (product.count ("vector.reduce"), 0u) << product.text ();
	EXPECT_TRUE (product.function->getReturnType ()->isFloatTy ()) << product.text ();
}

TEST_F (SimdNumericsBodies, ScalarMultiplySplatsAndMultipliesOnce)
{
	const Translation &scaled = translate (numerics, scalar_multiply);

	ASSERT_TRUE (scaled.error.empty ()) << scaled.error;
	ASSERT_NE (scaled.function, nullptr);

	Shape shape = shape_of (*scaled.function);

	EXPECT_EQ (shape.shuffles, 1u) << scaled.text ();
	EXPECT_EQ (shape.vector_fmuls, 1u) << scaled.text ();
	EXPECT_EQ (shape.scalar_fmuls, 0u) << scaled.text ();
	EXPECT_EQ (shape.calls, 0u) << scaled.text ();
}

} // namespace
} // namespace test
} // namespace mono
