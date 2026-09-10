/*
 * test-type-init-recursion.cpp: Unit test for the ordering bug fixed in
 * mono_runtime_class_init_full () (mono/metadata/object.c).
 *
 * Reporting a class's cctor failure builds a System.TypeInitializationException,
 * and building one runs managed code: that exception's own constructor. On
 * System.Globalization.CultureInfo, that constructor's call to
 * Environment.GetResourceString () reaches back into CultureInfo.CurrentCulture,
 * a class-init check on the very class whose failure is being reported --
 * which is what turned a real KSP install's CultureInfo class-init failure into
 * a stack overflow rather than a caught exception.
 *
 * A class-init check reentered on the same thread while do_initialization is
 * still running for it is meant to be safe: mono_runtime_class_init_full ()'s
 * own TypeInitializationLock finds the same thread already initializing and
 * returns at once, the ordinary reentrant-init rule ECMA-335 asks for.
 * init_failed used to go up before the wrapper exception existed to answer
 * with, so a reentrant call arriving in that window took the already-failed
 * branch instead, found nothing yet in type_init_exception_hash, and built a
 * second wrapper of its own -- running the same constructor, and reaching the
 * same window, again.
 *
 * mono_test_hook_type_init_exception_ctor reenters from exactly that window
 * without needing a class whose own failure is genuinely self-referential.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/exception-internals.h"
#include "metadata/object-internals.h"

#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace {

#define TESTPROG "type-init-recursion.exe"

MonoImage *g_image;

class TypeInitRecursion : public ::testing::Test {
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

	void TearDown () override { mono_test_hook_type_init_exception_ctor = nullptr; }
};

} // namespace

/*
 * The hook fires for every class whose failure is being reported, in whichever
 * test happens to run it first, so each case's own callback has to ignore any
 * name but the one it cares about.
 */
static MonoVTable *reentry_target;
static int hook_calls;
static gboolean reentrant_call_ok;

TEST_F (TypeInitRecursion, ReentrantReportDuringConstructionDoesNotRecurse)
{
	ERROR_DECL (error);
	MonoClass *klass = mono_class_from_name_checked (g_image, "", "TypeInitRecursion", error);

	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, klass);

	MonoVTable *vtable = mono_class_vtable_checked (mono_domain_get (), klass, error);
	mono_error_assert_ok (error);
	ASSERT_NE (nullptr, vtable);

	reentry_target = vtable;
	hook_calls = 0;
	reentrant_call_ok = FALSE;

	mono_test_hook_type_init_exception_ctor = [] (const gchar *type_name) {
		if (strstr (type_name, "TypeInitRecursion") == nullptr)
			return;

		/*
		 * One reentrant call is the scenario; never let the callback make a
		 * second one, so a build that still has the bug hits its own
		 * unbounded recursion in mono_runtime_class_init_full () rather than
		 * in this lambda.
		 */
		if (++hook_calls != 1)
			return;

		ERROR_DECL (reentrant_error);
		reentrant_call_ok = mono_runtime_class_init_full (reentry_target, reentrant_error)
		                     && is_ok (reentrant_error);
	};

	ERROR_DECL (init_error);
	gboolean ok = mono_runtime_class_init_full (vtable, init_error);

	EXPECT_FALSE (ok) << "TypeInitRecursion's cctor throws, so its class-init has to fail";
	EXPECT_EQ (1, hook_calls)
		<< "a reentrant call arriving while the wrapper exception was still "
		   "under construction built a second wrapper of its own instead of "
		   "taking the same-thread reentrant path";
	EXPECT_TRUE (reentrant_call_ok)
		<< "the reentrant call should find its own thread already "
		   "initializing this vtable and succeed at once";
}
