/*
 * test-mono-callspec.cpp: Unit test for the callspec parsing and evaluation.
 *
 * Copyright (C) 2017 vFunction, Inc.
 *
 */

// Embedders do not have the luxury of our config.h, so skip it here.
//#include "config.h"

// But we need MONO_INSIDE_RUNTIME to get MonoError mangled correctly
// because we also test unexported functions (mono_class_from_name_checked).
#define MONO_INSIDE_RUNTIME 1

#include "mono/utils/mono-publib.h"

// Allow to test external_only w/o deprecation error.
#undef MONO_RT_EXTERNAL_ONLY
#define MONO_RT_EXTERNAL_ONLY /* nothing */

#include <glib.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/callspec.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/assembly-internals.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/mini/jit.h>
#include <mono/utils/mono-error-internals.h>

#include <vector>

#include <gtest/gtest.h>

#include "harness.hpp"

#define TESTPROG "callspec.exe"

namespace {

enum test_method_enums {
	FOO_BAR,
	FOO_BARP,
	GOO_BAR,
	FOO2_BAR,
	CONSOLE_WRITELINE,
};

struct TestEntry {
	test_method_enums method;
	const char *callspec;
	gboolean expect_match;
};

MonoClass *
class_from_name (MonoImage *image, const char *name_space, const char *name)
{
	ERROR_DECL (error);
	MonoClass *klass;

	klass = mono_class_from_name_checked (image, name_space, name, error);
	mono_error_cleanup (error); /* FIXME Don't swallow the error */

	return klass;
}

/*
 * The methods the callspecs are matched against, loaded from callspec.exe and
 * from corlib.  The assembly comes up once per process alongside the runtime,
 * which gtest reaches again under --gtest_repeat.
 */
class Callspec : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CLASS_LIBRARY ();
		mono::test::init_runtime ();

		static bool loaded = false;
		if (loaded)
			return;
		loaded = true;

		MonoImageOpenStatus status;
		MonoAssembly *assembly = mono_assembly_open (TESTPROG, &status);
		ASSERT_NE (nullptr, assembly) << "failed loading " TESTPROG;

		mono_callspec_set_assembly (assembly);

		MonoImage *prog_image = mono_assembly_get_image_internal (assembly);

		ASSERT_NO_FATAL_FAILURE (add_method (prog_image, "Baz", "Foo", "Bar", 0));
		ASSERT_NO_FATAL_FAILURE (add_method (prog_image, "Baz", "Foo", "Bar", 1));
		ASSERT_NO_FATAL_FAILURE (add_method (prog_image, "Baz", "Goo", "Bar", 1));
		ASSERT_NO_FATAL_FAILURE (add_method (prog_image, "Baz", "Foo2", "Bar", 1));
		ASSERT_NO_FATAL_FAILURE (add_method (mono_get_corlib (), "System", "Console", "WriteLine", 1));
	}

protected:
	/* Every entry's method has to match the callspec exactly as the entry says. */
	static void check (const std::vector<TestEntry> &entries)
	{
		for (const TestEntry &entry : entries) {
			MonoMethod *method = test_methods [entry.method];
			char *method_name = mono_method_full_name (method, TRUE);
			ASSERT_NE (nullptr, method_name);

			MonoCallSpec spec = {0};
			char *errstr = NULL;
			ASSERT_TRUE (mono_callspec_parse (entry.callspec, &spec, &errstr))
				<< "parsing '" << entry.callspec << "': " << (errstr ? errstr : "");
			g_free (errstr);

			EXPECT_EQ (entry.expect_match, mono_callspec_eval (method, &spec))
				<< "matching '" << method_name << "' against '" << entry.callspec << "'";

			mono_callspec_cleanup (&spec);
			g_free (method_name);
		}
	}

private:
	static void add_method (MonoImage *image, const char *name_space, const char *name,
				const char *method_name, int param_count)
	{
		MonoClass *klass = class_from_name (image, name_space, name);
		ASSERT_NE (nullptr, klass) << "finding " << name_space << "." << name;

		MonoMethod *method = mono_class_get_method_from_name (klass, method_name, param_count);
		ASSERT_NE (nullptr, method)
			<< "finding " << name_space << "." << name << ":" << method_name
			<< " (" << param_count << " args)";

		test_methods.push_back (method);
	}

	static std::vector<MonoMethod *> test_methods;
};

std::vector<MonoMethod *> Callspec::test_methods;

} // namespace

TEST_F (Callspec, Program)
{
	check ({
		{FOO_BAR, "program", TRUE},
		{CONSOLE_WRITELINE, "program", FALSE},
		{FOO_BAR, "all,-program", FALSE},
		{CONSOLE_WRITELINE, "all,-program", TRUE},
	});
}

TEST_F (Callspec, Assembly)
{
	check ({
		{FOO_BAR, "mscorlib", FALSE},
		{CONSOLE_WRITELINE, "mscorlib", TRUE},
		{FOO_BAR, "all,-mscorlib", TRUE},
		{CONSOLE_WRITELINE, "all,-mscorlib", FALSE},
	});
}

TEST_F (Callspec, Class)
{
	check ({
		{FOO_BAR, "T:Baz.Foo", TRUE},
		{CONSOLE_WRITELINE, "T:Baz.Foo", FALSE},
		{FOO_BAR, "all,-T:Baz.Foo", FALSE},
		{CONSOLE_WRITELINE, "all,-T:Baz.Foo", TRUE},
	});
}

TEST_F (Callspec, Namespace)
{
	check ({
		{FOO_BAR, "N:Baz", TRUE},
		{CONSOLE_WRITELINE, "N:Baz", FALSE},
		{FOO_BAR, "all,-N:Baz", FALSE},
		{CONSOLE_WRITELINE, "all,-N:Baz", TRUE},
	});
}

/* A method spec with no parameter list matches every overload of the name. */
TEST_F (Callspec, MethodWithoutParameters)
{
	check ({
		{FOO_BAR, "M:Baz.Foo:Bar", TRUE},
		{FOO_BARP, "M:Baz.Foo:Bar", TRUE},
		{GOO_BAR, "M:Baz.Foo:Bar", FALSE},
		{FOO2_BAR, "M:Baz.Foo:Bar", FALSE},
		{CONSOLE_WRITELINE, "M:Baz.Foo:Bar", FALSE},
		{FOO_BAR, "all,-M:Baz.Foo:Bar", FALSE},
		{CONSOLE_WRITELINE, "all,-M:Baz.Foo:Bar", TRUE},
	});
}

/* ... and one with no class matches the name in any class. */
TEST_F (Callspec, MethodWithoutClass)
{
	check ({
		{FOO_BAR, "M::Bar", TRUE},
		{FOO_BARP, "M::Bar", TRUE},
		{GOO_BAR, "M::Bar", TRUE},
		{FOO2_BAR, "M::Bar", TRUE},
	});
}
