/*
 * test-detour.cpp: Unit test for handing a method's entry address to native
 * code.
 *
 * A native code patcher - Harmony, MonoMod, and the Unity mod code built on
 * them - replaces a method by taking the address it is entered at and making
 * calls arrive somewhere else. mono_install_method_detour () is the way to say
 * so: the entry is redirected at the target, the method never promotes again,
 * and a compile already running for it does not take the entry when it lands.
 *
 * A detour reaches a caller only where the caller goes through the entry. A
 * compiled caller always does. An interpreted one does when it makes a jit call
 * to the entry, which is what resolve_code_type () settles, and does not when
 * the interpreter has copied the callee's body into it.
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

#define TESTPROG "detour.exe"

/* What every detour in this file is pointed at, and what tells it apart from
 * the method's own body: Target () and Inlined () both answer x + 1. */
extern "C" int
detoured_body (int x)
{
	return x + 1000;
}

MonoImage *g_image;

class MethodDetour : public ::testing::Test {
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
		 * With tier 0 off every method is compiled, and the interpreted
		 * arms below then check nothing. Say so as a skip.
		 */
		if (!mono_llvm_jit_tier0_enabled ())
			GTEST_SKIP () << "tier 0 is off in this configuration";
	}

protected:
	/* Each case detours a method of its own, since a detour is never undone. */
	static MonoMethod *method_named (const char *name, int argc)
	{
		ERROR_DECL (error);
		MonoClass *klass =
			mono_class_from_name_checked (g_image, "", "Detour", error);

		mono_error_assert_ok (error);
		if (klass == nullptr)
			return nullptr;

		MonoMethod *found =
			mono_class_get_method_from_name_checked (klass, name, argc, 0, error);

		mono_error_assert_ok (error);
		return found;
	}

	/* The one instantiation over string of a one-type-parameter method. It is
	 * the same MonoMethod the caller's own token resolves to, since inflating
	 * is cached. */
	static MonoMethod *instantiated_over_string (MonoMethod *definition)
	{
		ERROR_DECL (error);
		MonoType *arguments[1] = { m_class_get_byval_arg (mono_get_string_class ()) };
		MonoGenericContext context;

		memset (&context, 0, sizeof (context));
		context.method_inst = mono_metadata_get_generic_inst (1, arguments);

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

	/*
	 * Waits up to a second for a promotion to land, and answers the body or
	 * null. A promotion that is taken lands in a few milliseconds, so this
	 * is generous for proving one did not happen.
	 */
	static void *await_body (MonoDomain *domain, MonoMethod *method)
	{
		for (int waited = 0; waited < 1000; ++waited) {
			if (void *body = mono_llvm_jit_find_body (domain, method))
				return body;
			g_usleep (1000);
		}
		return nullptr;
	}
};

} // namespace

/*
 * The entry address reaches the detour. This is the compiled caller's case:
 * a compiled call site names the stub, which is the address handed out here.
 */
TEST_F (MethodDetour, TakesTheEntryAddress)
{
	ERROR_DECL (error);
	MonoMethod *method = method_named ("Target", 1);

	ASSERT_NE (nullptr, method);

	int (*entry) (int) = (int (*) (int)) mono_compile_method_checked (method, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, entry);

	mono_install_method_detour (method, mono_domain_get (), (void *) detoured_body);

	EXPECT_EQ (1001, entry (1));
	/* The same address as before: a patcher's cached pointer still works. */
	EXPECT_EQ ((void *) entry, mono_compile_method_checked (method, error));
	mono_error_assert_ok (error);
}

/*
 * A detoured method does not promote. A body compiled after the detour would
 * take the entry back off the patcher, and the entry is what every caller has.
 */
TEST_F (MethodDetour, OutranksAPromotion)
{
	ERROR_DECL (error);
	MonoDomain *domain = mono_domain_get ();
	MonoMethod *method = method_named ("Inlined", 1);

	ASSERT_NE (nullptr, method);
	ASSERT_GT (mono_llvm_jit_tier0_calls (method), 0)
		<< "this method no longer starts at tier 0, so it checks nothing";

	mono_install_method_detour (method, domain, (void *) detoured_body);

	int (*entry) (int) = (int (*) (int)) mono_compile_method_checked (method, error);
	mono_error_assert_ok (error);
	ASSERT_EQ (1001, entry (1)) << "the detour was never installed";

	/*
	 * True because there is nothing left for the counting engine to do, not
	 * because a request went out. Nothing must reach the compile queue.
	 */
	EXPECT_TRUE (mono_promote_method (method, domain));
	EXPECT_EQ (nullptr, await_body (domain, method));
}

/*
 * An interpreted caller reaches the detour, because resolve_code_type () sees
 * the entry is native and makes a jit call to it rather than interpreting the
 * callee.
 */
TEST_F (MethodDetour, IsSeenByAnInterpretedCaller)
{
	MonoMethod *target = method_named ("Target", 1);
	MonoMethod *caller = method_named ("CallTarget", 1);

	ASSERT_NE (nullptr, target);
	ASSERT_NE (nullptr, caller);
	ASSERT_GT (mono_llvm_jit_tier0_calls (caller), 0)
		<< "the caller no longer starts at tier 0, so it is not interpreted";
	ASSERT_GT (mono_llvm_jit_tier0_calls (target), 0)
		<< "the callee already has code, so this checks a compiled call";

	mono_install_method_detour (target, mono_domain_get (), (void *) detoured_body);

	EXPECT_EQ (1001, invoke (caller, 1));
}

/*
 * So does an interpreted caller of a generic method. An instantiation is
 * entered exactly like any other method - its entry supplies whatever context
 * the body behind it reads - so the interpreter has the same two choices for it
 * and takes the same one.
 *
 * The detour is the instrument rather than the subject here. It is what makes
 * the answer say which choice was taken, and it needs no promotion to land
 * first, so this arm is not racing a background compile.
 */
TEST_F (MethodDetour, IsSeenByAnInterpretedCallerOfAnInstantiation)
{
	MonoMethod *definition = method_named ("GenericTarget", 1);
	MonoMethod *caller = method_named ("CallGenericTarget", 1);

	ASSERT_NE (nullptr, definition);
	ASSERT_NE (nullptr, caller);

	MonoMethod *target = instantiated_over_string (definition);

	ASSERT_NE (nullptr, target);
	ASSERT_GT (mono_llvm_jit_tier0_calls (caller), 0)
		<< "the caller no longer starts at tier 0, so it is not interpreted";
	ASSERT_GT (mono_llvm_jit_tier0_calls (target), 0)
		<< "the callee already has code, so this checks a compiled call";

	mono_install_method_detour (target, mono_domain_get (), (void *) detoured_body);

	EXPECT_EQ (1001, invoke (caller, 1));
}

/*
 * A callee the interpreter copied into its caller does not. The copy is not
 * reached through the entry, and nothing rewrites a body already transformed.
 *
 * This is the documented limitation, asserted so that putting the record's stub
 * on the interpreted call path has a case that flips.
 */
TEST_F (MethodDetour, IsMissedByAnInlinedCallee)
{
	ERROR_DECL (error);
	MonoMethod *target = method_named ("Inlined", 1);
	MonoMethod *caller = method_named ("CallInlined", 1);

	ASSERT_NE (nullptr, target);
	ASSERT_NE (nullptr, caller);
	ASSERT_GT (mono_llvm_jit_tier0_calls (caller), 0)
		<< "the caller no longer starts at tier 0, so it is not interpreted";

	mono_install_method_detour (target, mono_domain_get (), (void *) detoured_body);

	/* The detour is there. What follows is about the path, not the install. */
	int (*entry) (int) = (int (*) (int)) mono_compile_method_checked (target, error);
	mono_error_assert_ok (error);
	ASSERT_EQ (1001, entry (1));

	EXPECT_EQ (2, invoke (caller, 1));
}
