/*
 * Tests for the bodies method-to-llvm/simd-vector-t.cpp writes in place of
 * System.Numerics.Vector<T>'s managed ones.
 *
 * Vector`1 is declared in mscorlib, and the image holds the generic definition
 * alone, so a method desc cannot name one of these methods. Each case binds the
 * type argument itself and translates the method off the instantiation.
 */

#include "harness.hpp"

#include "cl-opt-override.hpp"
#include "method-to-llvm.hpp"
#include "method-to-llvm/intrinsics.hpp"
#include "runtime/minimal-compile.hpp"

#include <glib.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/domain-internals.h>
#include <mono/metadata/object-internals.h>
#include <mono/utils/mono-error-internals.h>

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace mono {
namespace test {
namespace {

/// Instruction counts read off one translated function.
struct Shape {
	unsigned instructions = 0;
	unsigned calls = 0;
	unsigned vector_adds = 0;
	unsigned vector_fadds = 0;
	unsigned vector_divides = 0;
	unsigned vector_fcmps = 0;
	unsigned selects = 0;
	unsigned sexts = 0;
};

Shape
shape_of (llvm::Function &function)
{
	Shape shape;

	for (llvm::Instruction &instruction : llvm::instructions (function)) {
		++shape.instructions;

		if (llvm::isa<llvm::CallBase> (instruction))
			++shape.calls;

		bool vector = instruction.getNumOperands () > 0
		              && instruction.getOperand (0)->getType ()->isVectorTy ();

		switch (instruction.getOpcode ()) {
		case llvm::Instruction::Add:
			shape.vector_adds += vector;
			break;
		case llvm::Instruction::FAdd:
			shape.vector_fadds += vector;
			break;
		case llvm::Instruction::SDiv:
		case llvm::Instruction::UDiv:
			shape.vector_divides += vector;
			break;
		case llvm::Instruction::FCmp:
			shape.vector_fcmps += vector;
			break;
		case llvm::Instruction::Select:
			++shape.selects;
			break;
		case llvm::Instruction::SExt:
			++shape.sexts;
			break;
		default:
			break;
		}
	}

	return shape;
}

MonoMethod *
member_of (MonoClass *klass, const char *name, int params)
{
	ERROR_DECL (lookup);
	MonoMethod *method =
		mono_class_get_method_from_name_checked (klass, name, params, 0, lookup);

	mono_error_cleanup (lookup);
	return method;
}

/// Vector<element>, or null where corlib does not declare Vector`1.
MonoClass *
vector_of (MonoClass *element)
{
	ERROR_DECL (lookup);
	MonoClass *definition = mono_class_from_name_checked (
		mono_get_corlib (), "System.Numerics", "Vector`1", lookup);

	mono_error_cleanup (lookup);

	if (definition == nullptr)
		return nullptr;

	MonoType *arguments[1] = { m_class_get_byval_arg (element) };

	return mono_class_bind_generic_parameters (definition, 1, arguments, FALSE);
}

class VectorTBodies : public TranslatorTest {
protected:
	/// Translate Vector<element>'s method of that name and arity.
	///
	/// Aborts on a name no such method answers to, the way the harness does
	/// for one a method desc cannot find.
	const Translation &translate_member (MonoClass *element, const char *name,
	                                     int params)
	{
		MonoClass *klass = vector_of (element);

		if (klass == nullptr)
			g_error ("no System.Numerics.Vector`1 in mscorlib");

		MonoMethod *method = member_of (klass, name, params);

		if (method == nullptr)
			g_error ("no %s of %d parameters on Vector`1", name, params);

		auto owned = std::make_unique<Translation> ();
		Translation &result = *owned;

		result.context = std::make_unique<llvm::LLVMContext> ();
		result.module = std::make_unique<llvm::Module> (name, *result.context);

		ERROR_DECL (metadata_error);
		MinimalCompile cfg (method, mono_domain_get (), metadata_error);

		if (cfg.get ()->header == nullptr) {
			result.error = mono_error_get_message (metadata_error);
			mono_error_cleanup (metadata_error);
		} else if (llvm::Expected<llvm::Function *> translated = method_to_llvm (
				   result.module.get (), cfg.get (), method)) {
			result.function = *translated;
			result.verifier_error = verify_function (*result.function);
		} else {
			result.error = llvm::toString (translated.takeError ());
		}

		owned_translations.push_back (std::move (owned));
		return *owned_translations.back ();
	}

	void TearDown () override
	{
		for (const auto &translation : owned_translations)
			if (!translation->verifier_error.empty ())
				ADD_FAILURE () << "the emitted IR does not verify:\n"
					       << translation->verifier_error << "\n"
					       << translation->text ();

		TranslatorTest::TearDown ();
	}

private:
	std::vector<std::unique_ptr<Translation>> owned_translations;
};

TEST_F (VectorTBodies, IntegerAddIsOneVectorAdd)
{
	const Translation &added =
		translate_member (mono_get_int32_class (), "op_Addition", 2);

	ASSERT_TRUE (added.error.empty ()) << added.error;
	ASSERT_NE (added.function, nullptr);

	Shape shape = shape_of (*added.function);

	EXPECT_EQ (shape.vector_adds, 1u) << added.text ();
	EXPECT_EQ (shape.calls, 0u) << added.text ();
	EXPECT_EQ (shape.instructions, 2u) << added.text ();
	EXPECT_EQ (added.count ("add <4 x i32>"), 1u) << added.text ();
}

TEST_F (VectorTBodies, FloatAddIsOneVectorFadd)
{
	const Translation &added =
		translate_member (mono_get_single_class (), "op_Addition", 2);

	ASSERT_TRUE (added.error.empty ()) << added.error;
	ASSERT_NE (added.function, nullptr);

	Shape shape = shape_of (*added.function);

	EXPECT_EQ (shape.vector_fadds, 1u) << added.text ();
	EXPECT_EQ (shape.calls, 0u) << added.text ();
	EXPECT_EQ (shape.instructions, 2u) << added.text ();
	EXPECT_EQ (added.count ("fadd <4 x float>"), 1u) << added.text ();
}

// A zero lane makes the managed body throw and sdiv poison, so the row declines
// an integer T.
TEST_F (VectorTBodies, IntegerDivideKeepsItsManagedBody)
{
	const Translation &divided =
		translate_member (mono_get_int32_class (), "op_Division", 2);

	ASSERT_TRUE (divided.error.empty ()) << divided.error;
	ASSERT_NE (divided.function, nullptr);

	Shape shape = shape_of (*divided.function);

	EXPECT_EQ (shape.vector_divides, 0u) << divided.text ();
	EXPECT_GT (shape.instructions, 2u);
}

TEST_F (VectorTBodies, FloatMinIsACompareAndASelect)
{
	const Translation &smallest = translate_member (mono_get_single_class (), "Min", 2);

	ASSERT_TRUE (smallest.error.empty ()) << smallest.error;
	ASSERT_NE (smallest.function, nullptr);

	Shape shape = shape_of (*smallest.function);

	EXPECT_EQ (shape.vector_fcmps, 1u) << smallest.text ();
	EXPECT_EQ (shape.selects, 1u) << smallest.text ();
	EXPECT_EQ (shape.calls, 0u) << smallest.text ();
	EXPECT_EQ (smallest.count ("fcmp olt <4 x float>"), 1u) << smallest.text ();
}

TEST_F (VectorTBodies, FloatLessThanIsACompareWidenedToAMask)
{
	const Translation &mask =
		translate_member (mono_get_single_class (), "LessThan", 2);

	ASSERT_TRUE (mask.error.empty ()) << mask.error;
	ASSERT_NE (mask.function, nullptr);

	Shape shape = shape_of (*mask.function);

	EXPECT_EQ (shape.vector_fcmps, 1u) << mask.text ();
	EXPECT_EQ (shape.sexts, 1u) << mask.text ();
	EXPECT_EQ (shape.calls, 0u) << mask.text ();
	EXPECT_EQ (mask.count ("sext <4 x i1>"), 1u) << mask.text ();
}

TEST_F (VectorTBodies, LoweringOffTranslatesTheManagedBody)
{
	BoolOptionOverride off ("mono-simd", false);
	const Translation &added =
		translate_member (mono_get_int32_class (), "op_Addition", 2);

	ASSERT_TRUE (added.error.empty ()) << added.error;
	ASSERT_NE (added.function, nullptr);

	Shape shape = shape_of (*added.function);

	EXPECT_EQ (shape.vector_adds, 0u) << added.text ();
	EXPECT_GT (shape.instructions, 2u);
}

// The IL computes the same lanes, so tier 0 can keep running it.
TEST_F (VectorTBodies, ALoweredMemberStillRunsItsOwnIl)
{
	MonoClass *klass = vector_of (mono_get_int32_class ());

	ASSERT_NE (klass, nullptr);

	MonoMethod *method = member_of (klass, "op_Addition", 2);

	ASSERT_NE (method, nullptr);
	EXPECT_FALSE (builtin_body_replaces_il (method));
}

} // namespace
} // namespace test
} // namespace mono
