/*
 * test-il-analyzer.cpp: Unit test for analyze_il_reachability ().
 *
 * Exercises reachability analysis for exact and shared generic
 * instantiations.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/metadata-internals.h"
#include "mini/mini.h"

#include "llvm/runtime/il-analyzer.hpp"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/metadata.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace {

#define TESTPROG "il-analyzer.exe"

MonoImage *g_image;

class ILAnalyzer : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
		mono::test::init_runtime ();

		if (g_image != nullptr)
			return;

		MonoAssemblyOpenRequest req;
		mono_assembly_request_prepare_open (
			&req, MONO_ASMCTX_DEFAULT,
			mono_domain_default_alc (mono_domain_get ()));

		MonoImageOpenStatus status;
		MonoAssembly *assembly = mono_assembly_request_open (TESTPROG, &req, &status);

		ASSERT_NE (nullptr, assembly) << "failed loading " TESTPROG;
		g_image = mono_assembly_get_image_internal (assembly);
	}

	void SetUp () override { MONO_SKIP_WITHOUT_CLASS_LIBRARY (); }

protected:
	static MonoMethod *method_named (const char *name, int argc)
	{
		ERROR_DECL (error);
		MonoClass *klass = mono_class_from_name_checked (g_image, "", "Analyzed", error);

		mono_error_assert_ok (error);
		if (klass == nullptr)
			return nullptr;

		MonoMethod *found =
			mono_class_get_method_from_name_checked (klass, name, argc, 0, error);

		mono_error_assert_ok (error);
		return found;
	}

	/// The generic method name instantiated over klass.
	static MonoMethod *instantiated (const char *name, MonoClass *klass)
	{
		MonoMethod *generic = method_named (name, 0);

		if (generic == nullptr)
			return nullptr;

		MonoType *arg = m_class_get_byval_arg (klass);
		MonoGenericContext context = {};

		context.method_inst = mono_metadata_get_generic_inst (1, &arg);

		ERROR_DECL (error);
		MonoMethod *inflated = mono_class_inflate_generic_method_checked (generic, &context, error);

		mono_error_assert_ok (error);
		return inflated;
	}

	/// The body every reference instantiation of name runs.
	static MonoMethod *shared (const char *name)
	{
		MonoMethod *over_object = instantiated (name, mono_defaults.object_class);

		if (over_object == nullptr)
			return nullptr;

		ERROR_DECL (error);
		MonoMethod *form = mini_get_shared_method_full (over_object, SHARE_MODE_NONE, error);

		mono_error_assert_ok (error);
		return form;
	}

	struct Walked {
		uint32_t code_size;
		mono::ILReachability reach;
	};

	static std::optional<Walked> walk (MonoMethod *method)
	{
		if (method == nullptr)
			return std::nullopt;

		ERROR_DECL (error);
		MonoMethodHeader *header = mono_method_get_header_checked (method, error);

		mono_error_assert_ok (error);
		if (header == nullptr)
			return std::nullopt;

		std::optional<mono::ILReachability> reach = mono::analyze_il_reachability (method, header);
		std::optional<Walked> walked;

		if (reach)
			walked = Walked { header->code_size, *reach };

		mono_metadata_free_mh (header);
		return walked;
	}
};

} // namespace

/* A body with no branch is live from end to end. */
TEST_F (ILAnalyzer, StraightLineIsAllLive)
{
	std::optional<Walked> walked = walk (method_named ("Straight", 1));

	ASSERT_TRUE (walked.has_value ());
	EXPECT_EQ (walked->code_size, walked->reach.live_bytes);
	EXPECT_EQ (0u, walked->reach.decided_branches);
}

/*
 * typeof (T) == typeof (int) is decided for every instantiation: true for
 * int, which loses the padded arm, and false for double, which loses the
 * early return. The shared body every reference instantiation runs answers
 * false as well, because a reference type is never int.
 */
TEST_F (ILAnalyzer, TypeofGuardDecidesForEachInstantiation)
{
	std::optional<Walked> over_int = walk (instantiated ("Guard", mono_defaults.int32_class));
	std::optional<Walked> over_double = walk (instantiated ("Guard", mono_defaults.double_class));
	std::optional<Walked> over_reference = walk (shared ("Guard"));

	ASSERT_TRUE (over_int && over_double && over_reference);

	EXPECT_EQ (1u, over_int->reach.decided_branches);
	EXPECT_LT (over_int->reach.live_bytes, over_double->reach.live_bytes);

	EXPECT_EQ (1u, over_double->reach.decided_branches);
	EXPECT_LT (over_double->reach.live_bytes, over_double->code_size);

	EXPECT_EQ (1u, over_reference->reach.decided_branches);
	EXPECT_EQ (over_double->reach.live_bytes, over_reference->reach.live_bytes);
}

/* Type.IsValueType is false for the shared form and true for a value type. */
TEST_F (ILAnalyzer, IsValueTypeDecidesForSharedForm)
{
	std::optional<Walked> over_int = walk (instantiated ("IsReference", mono_defaults.int32_class));
	std::optional<Walked> over_reference = walk (shared ("IsReference"));

	ASSERT_TRUE (over_int && over_reference);

	EXPECT_EQ (1u, over_int->reach.decided_branches);
	EXPECT_LT (over_int->reach.live_bytes, over_int->code_size);

	EXPECT_EQ (1u, over_reference->reach.decided_branches);
	EXPECT_LT (over_reference->reach.live_bytes, over_int->reach.live_bytes);
}

/*
 * Boxing int produces a non-null object, so its branch is known. The shared
 * form depends on the runtime context and remains unknown.
 */
TEST_F (ILAnalyzer, BoxOfValueTypeIsNonNull)
{
	std::optional<Walked> over_int = walk (instantiated ("Boxed", mono_defaults.int32_class));
	std::optional<Walked> over_reference = walk (shared ("Boxed"));

	ASSERT_TRUE (over_int && over_reference);

	EXPECT_EQ (1u, over_int->reach.decided_branches);
	EXPECT_LT (over_int->reach.live_bytes, over_int->code_size);

	EXPECT_EQ (0u, over_reference->reach.decided_branches);
	EXPECT_EQ (over_reference->code_size, over_reference->reach.live_bytes);
}

/*
 * Constants propagated through locals allow the switch to select one case.
 */
TEST_F (ILAnalyzer, SwitchOnFoldedLocalKeepsOneCase)
{
	std::optional<Walked> over_int = walk (instantiated ("Switched", mono_defaults.int32_class));
	std::optional<Walked> over_long = walk (instantiated ("Switched", mono_defaults.int64_class));
	std::optional<Walked> over_reference = walk (shared ("Switched"));

	ASSERT_TRUE (over_int && over_long && over_reference);

	EXPECT_LT (over_int->reach.live_bytes, over_reference->reach.live_bytes);
	EXPECT_LT (over_long->reach.live_bytes, over_reference->reach.live_bytes);
	EXPECT_LT (over_reference->reach.live_bytes, over_reference->code_size);

	// The first guard settles int and the switch follows it. long falls
	// through the first guard to the second.
	EXPECT_EQ (2u, over_int->reach.decided_branches);
	EXPECT_EQ (3u, over_long->reach.decided_branches);
}

/* A guard inside a try is decided the same, and the finally stays live. */
TEST_F (ILAnalyzer, GuardInsideTryIsDecided)
{
	std::optional<Walked> over_int = walk (instantiated ("Protected", mono_defaults.int32_class));
	std::optional<Walked> over_double = walk (instantiated ("Protected", mono_defaults.double_class));

	ASSERT_TRUE (over_int && over_double);

	EXPECT_EQ (1u, over_int->reach.decided_branches);
	EXPECT_LT (over_int->reach.live_bytes, over_double->reach.live_bytes);
	EXPECT_LT (over_double->reach.live_bytes, over_double->code_size);
}

/* A local a loop writes is unknown where the loop meets the code after it. */
TEST_F (ILAnalyzer, LocalWrittenInLoopIsUnknown)
{
	std::optional<Walked> walked = walk (method_named ("Looped", 1));

	ASSERT_TRUE (walked.has_value ());
	EXPECT_EQ (walked->code_size, walked->reach.live_bytes);
}

/* A local whose address is taken is unknown, whatever was stored in it. */
TEST_F (ILAnalyzer, LocalWithAddressTakenIsUnknown)
{
	std::optional<Walked> walked = walk (method_named ("AddressTaken", 0));

	ASSERT_TRUE (walked.has_value ());
	EXPECT_EQ (walked->code_size, walked->reach.live_bytes);
	EXPECT_EQ (0u, walked->reach.decided_branches);
}
