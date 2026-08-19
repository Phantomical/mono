/*
 * test-method-override.cpp: Unit test for replacing a method with another
 * managed method.
 *
 * mono_install_method_override () differs from a detour in covering both
 * engines. A compiled caller reaches the replacement through the entry, which a
 * detour does too; an interpreted caller runs the replacement's bytecode, which
 * a detour manages only where mono_interp_jit_call_marshallable () agrees. The
 * cases below that a detour cannot pass are the generic one and the wide one.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/metadata-internals.h"
#include "metadata/object-internals.h"
#include "mini/domain-method.h"
#include "mini/jit.h"

#include "llvm/runtime.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace {

#define TESTPROG "method-override.exe"

MonoImage *g_image;

class MethodOverride : public ::testing::Test {
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

	void SetUp () override
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();

		/*
		 * With tier 0 off every method is compiled, and the interpreted arms
		 * below then check nothing. Say so as a skip.
		 */
		if (!mono_llvm_jit_tier0_enabled ())
			GTEST_SKIP () << "tier 0 is off in this configuration";
	}

protected:
	static MonoMethod *method_named (const char *name, int argc)
	{
		ERROR_DECL (error);
		MonoClass *klass = mono_class_from_name_checked (g_image, "", "Override", error);

		mono_error_assert_ok (error);
		if (klass == nullptr)
			return nullptr;

		MonoMethod *found =
			mono_class_get_method_from_name_checked (klass, name, argc, 0, error);

		mono_error_assert_ok (error);
		return found;
	}

	/// The named generic method with its one type parameter bound to int.
	static MonoMethod *method_of_int (const char *name, int argc)
	{
		ERROR_DECL (error);
		MonoMethod *definition = method_named (name, argc);

		if (definition == nullptr)
			return nullptr;

		MonoType *arg = m_class_get_byval_arg (mono_defaults.int32_class);
		MonoGenericContext context = { nullptr,
			                       mono_metadata_get_generic_inst (1, &arg) };
		MonoMethod *inflated =
			mono_class_inflate_generic_method_checked (definition, &context, error);

		mono_error_assert_ok (error);
		return inflated;
	}

	/* What a managed caller answers, run through an ordinary invoke. */
	static int invoke (MonoMethod *method, int argument)
	{
		ERROR_DECL (error);
		void *args[1] = { &argument };
		MonoObject *result = mono_runtime_invoke_checked (method, nullptr, args, error);

		mono_error_assert_ok (error);
		EXPECT_NE (nullptr, result);
		return result != nullptr ? *(int *) mono_object_unbox_internal (result) : 0;
	}
};

} // namespace

/*
 * The entry address reaches the replacement, and it is the address a caller
 * already holds rather than a new one.
 */
TEST_F (MethodOverride, TakesTheEntryAddress)
{
	ERROR_DECL (error);
	MonoMethod *target = method_named ("Target", 1);
	MonoMethod *replacement = method_named ("Replacement", 1);

	ASSERT_NE (nullptr, target);
	ASSERT_NE (nullptr, replacement);

	int (*entry) (int) = (int (*) (int)) mono_compile_method_checked (target, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, entry);

	mono_install_method_override (target, mono_domain_get (), replacement);

	EXPECT_EQ (1001, entry (1));
	EXPECT_EQ ((void *) entry, mono_compile_method_checked (target, error));
	mono_error_assert_ok (error);
}

/*
 * A second override replaces the first. Harmony re-points the same method on
 * every patch and on every unpatch, so a method three mods patch is re-pointed
 * five or six times.
 */
TEST_F (MethodOverride, IsRetargetable)
{
	ERROR_DECL (error);
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *target = method_named ("Retargeted", 1);

	ASSERT_NE (nullptr, target);

	int (*entry) (int) = (int (*) (int)) mono_compile_method_checked (target, error);
	mono_error_assert_ok (error);

	mono_install_method_override (target, domain, method_named ("Replacement", 1));
	ASSERT_EQ (1001, entry (1));

	mono_install_method_override (target, domain, method_named ("SecondReplacement", 1));
	EXPECT_EQ (2001, entry (1));
	EXPECT_EQ (2001, invoke (target, 1));
}

/* An interpreted caller runs the replacement's bytecode. */
TEST_F (MethodOverride, IsSeenByAnInterpretedCaller)
{
	MonoMethod *target = method_named ("Target", 1);
	MonoMethod *caller = method_named ("CallTarget", 1);

	ASSERT_NE (nullptr, target);
	ASSERT_NE (nullptr, caller);
	ASSERT_GT (mono_llvm_jit_tier0_calls (caller), 0)
		<< "the caller no longer starts at tier 0, so it is not interpreted";

	mono_install_method_override (target, mono_domain_get (), method_named ("Replacement", 1));

	EXPECT_EQ (1001, invoke (caller, 1));
}

/*
 * A generic method, which a detour cannot reach from an interpreted caller:
 * mono_interp_jit_call_marshallable () refuses an inflated method outright.
 */
TEST_F (MethodOverride, ReachesAnInflatedMethod)
{
	MonoMethod *target = method_of_int ("Generic", 1);
	MonoMethod *replacement = method_of_int ("GenericReplacement", 1);
	MonoMethod *caller = method_named ("CallGeneric", 1);

	ASSERT_NE (nullptr, target);
	ASSERT_NE (nullptr, replacement);
	ASSERT_NE (nullptr, caller);
	ASSERT_TRUE (target->is_inflated) << "this target no longer checks the inflated case";

	mono_install_method_override (target, mono_domain_get (), replacement);

	EXPECT_EQ (0, invoke (caller, 7));
}

/*
 * Eight parameters, where the same predicate refuses more than six. The caller
 * takes one argument and passes zeroes, so the answer still says which body ran.
 */
TEST_F (MethodOverride, ReachesAWideSignature)
{
	MonoMethod *target = method_named ("Wide", 8);
	MonoMethod *caller = method_named ("CallWide", 1);

	ASSERT_NE (nullptr, target);
	ASSERT_NE (nullptr, caller);
	ASSERT_GT (mono_method_signature_internal (target)->param_count, 6)
		<< "this target no longer checks the wide case";

	mono_install_method_override (target, mono_domain_get (),
	                              method_named ("WideReplacement", 8));

	EXPECT_EQ (1000, invoke (caller, 7));
}

/*
 * A callee small enough to be copied into its caller is reached anyway, because
 * overriding it marks it NoInlining and the interpreter reads that while it
 * transforms each caller. A caller transformed before the override keeps what
 * it copied, which is why this one is not called first.
 */
TEST_F (MethodOverride, IsNotInlinedAway)
{
	MonoMethod *target = method_named ("Small", 1);
	MonoMethod *caller = method_named ("CallSmall", 1);

	ASSERT_NE (nullptr, target);
	ASSERT_NE (nullptr, caller);

	mono_install_method_override (target, mono_domain_get (), method_named ("Replacement", 1));

	EXPECT_EQ (1001, invoke (caller, 1));
}
