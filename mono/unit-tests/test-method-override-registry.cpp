/*
 * test-method-override-registry.cpp: Unit test for the override assembly.
 *
 * Every assembly marked [assembly: MonoOverrideAssembly] is read as it loads,
 * and the methods its attributes name are replaced in whatever image they turn
 * out to live in. The cases below cover what a plain table lookup gets wrong: a
 * target loaded twice under two assembly names, which is what a process full of
 * Harmony repacks looks like; a generic target, whose instantiations do not
 * exist when the assembly is read; and an assembly that declares its own copies
 * of the attributes, which is what one built against another mscorlib has to
 * do.
 */

#include "config.h"

#include "metadata/class-internals.h"
#include "metadata/metadata-internals.h"
#include "metadata/object-internals.h"
#include "mini/method-override.hpp"

#include "llvm/runtime.h"

#include <mono/metadata/appdomain.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/object.h>
#include <mono/utils/mono-error-internals.h>

#include <gtest/gtest.h>

#include "harness.hpp"

namespace {

#define REGISTRY "override-registry.exe"
#define TARGET_A "override-target-a.exe"
#define TARGET_B "override-target-b.exe"

MonoImage *g_a;
MonoImage *g_b;

class MethodOverrideRegistry : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
		mono::test::init_runtime ();

		if (g_a != nullptr)
			return;

		/*
		 * Before the targets, the way the runtime preloads
		 * mono-overrides.dll - at the end of mini_init (), when nothing
		 * an override can name has run.
		 */
		ASSERT_TRUE (mono::method_overrides_preload (REGISTRY))
			<< "failed reading " REGISTRY;

		g_a = load (TARGET_A);
		g_b = load (TARGET_B);
	}

	void SetUp () override
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();

		/*
		 * With tier 0 off every method is compiled, and the interpreted
		 * callers below then check nothing. Say so as a skip.
		 */
		if (!mono_llvm_jit_tier0_enabled ())
			GTEST_SKIP () << "tier 0 is off in this configuration";
	}

protected:
	static MonoImage *load (const char *name)
	{
		MonoAssemblyOpenRequest req;
		mono_assembly_request_prepare_open (
			&req, MONO_ASMCTX_DEFAULT,
			mono_domain_default_alc (mono_domain_get ()));

		MonoImageOpenStatus status;
		MonoAssembly *assembly = mono_assembly_request_open (name, &req, &status);

		EXPECT_NE (nullptr, assembly) << "failed loading " << name;
		return assembly != nullptr ? mono_assembly_get_image_internal (assembly)
		                           : nullptr;
	}

	static MonoMethod *method_named (MonoImage *image, const char *name, int argc)
	{
		ERROR_DECL (error);
		MonoClass *klass =
			mono_class_from_name_checked (image, "Mono.Test", "Target", error);

		mono_error_assert_ok (error);
		if (klass == nullptr)
			return nullptr;

		MonoMethod *found =
			mono_class_get_method_from_name_checked (klass, name, argc, 0, error);

		mono_error_assert_ok (error);
		return found;
	}

	static void invoke (MonoMethod *method)
	{
		ERROR_DECL (error);

		mono_runtime_invoke_checked (method, nullptr, nullptr, error);
		mono_error_assert_ok (error);
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
 * Both loaded copies of the target are replaced. Matching is by name for this
 * reason: each mod ships its own repack of the assembly it patches, so several
 * copies of one type are loaded at once and every one of them has to be caught.
 */
TEST_F (MethodOverrideRegistry, ReplacesEveryLoadedCopy)
{
	MonoMethod *a = method_named (g_a, "Value", 1);
	MonoMethod *b = method_named (g_b, "Value", 1);

	ASSERT_NE (nullptr, a);
	ASSERT_NE (nullptr, b);
	ASSERT_NE (a, b) << "the two assemblies share a method, so this checks one copy";

	EXPECT_EQ (101, invoke (a, 1));
	EXPECT_EQ (101, invoke (b, 1));
}

/*
 * A caller that copies its callee's body into itself copies the replacement's,
 * because the interpreter settles each site's callee through the override table
 * before it decides to inline.
 */
TEST_F (MethodOverrideRegistry, InlinesTheReplacement)
{
	MonoMethod *caller = method_named (g_a, "CallValue", 1);

	ASSERT_NE (nullptr, caller);
	EXPECT_EQ (101, invoke (caller, 1));
}

/*
 * A generic target. Its instantiations do not exist when the override assembly
 * is read, so each is caught as its record is built and the replacement is
 * inflated with the target's own type arguments.
 */
TEST_F (MethodOverrideRegistry, ReachesAGenericTarget)
{
	MonoMethod *caller = method_named (g_a, "CallGeneric", 1);

	ASSERT_NE (nullptr, caller);
	EXPECT_EQ (201, invoke (caller, 1));
}

/* Eight parameters, which a detour cannot carry into an interpreted caller. */
TEST_F (MethodOverrideRegistry, ReachesAWideSignature)
{
	MonoMethod *caller = method_named (g_a, "CallWide", 1);

	ASSERT_NE (nullptr, caller);
	EXPECT_EQ (301, invoke (caller, 1));
}

/*
 * Mono.Overrides.MonoOverride::Install, which is how an override assembly
 * replaces a method it cannot name in an attribute - Harmony hands the
 * runtime a MethodBase it was given at run time. The caller is transformed
 * after the install, so its site names the replacement.
 */
TEST_F (MethodOverrideRegistry, IsReachableThroughTheIcall)
{
	MonoMethod *install = method_named (g_a, "InstallHandled", 0);
	MonoMethod *caller = method_named (g_a, "CallHandled", 1);

	ASSERT_NE (nullptr, install);
	ASSERT_NE (nullptr, caller);

	invoke (install);
	EXPECT_EQ (401, invoke (caller, 1));
}

/*
 * An assembly that carries the assembly attribute is read as it loads, however
 * late that is, and it can name a target inside itself. Both attributes are its
 * own rather than corlib's, which is the case for an assembly built against a
 * different mscorlib.
 */
TEST_F (MethodOverrideRegistry, ReadsAnAssemblyThatDeclaresItsOwn)
{
	MonoMethod *caller = method_named (g_a, "CallSelfDeclared", 1);

	ASSERT_NE (nullptr, caller);
	EXPECT_EQ (501, invoke (caller, 1));
}
