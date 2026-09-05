#include "config.h"

#include <glib.h>
#include <mono/metadata/assembly.h>
#include <mono/mini/jit.h>

#include <vector>

#include "harness.hpp"

namespace mono {
namespace test {

// Where the class libraries are.  The build says, because a test binary can be
// started from anywhere.
#define CLASS_LIBRARY_DIR MONO_UNIT_TESTS_ASSEMBLIES

/*
 * Applies MONO_ENV_OPTIONS the way mono_main () does for the real runtime,
 * which mono_jit_init_version_for_test_only () does not. The CTest registration
 * puts a case's tier-0 engine there.
 */
static void
apply_env_options ()
{
	const char *env = getenv ("MONO_ENV_OPTIONS");

	if (env == nullptr)
		return;

	/* Kept for the life of the process, as mono_main ()'s argv is: the parser
	 * hands some of these strings on rather than copying them. */
	gchar **tokens = g_strsplit (env, " ", -1);
	std::vector<char *> argv;

	for (gchar **token = tokens; *token != nullptr; ++token)
		if (**token != '\0')
			argv.push_back (*token);

	mono_jit_parse_options ((int) argv.size (), argv.data ());
}

/*
 * Every suite in the binary shares this one runtime, so which of them gets here
 * first cannot matter -- and it varies, since ctest runs each case on its own
 * and --gtest_shuffle reorders the rest.
 */
void
init_runtime ()
{
	static bool started = false;

	if (started)
		return;
	started = true;

	apply_env_options ();

	//FIXME This is a hack due to embedding simply not working from the tree
	mono_set_assemblies_path (CLASS_LIBRARY_DIR);
	mono_jit_init_version_for_test_only ("mono-unit-tests", "v4.0.30319");
}

bool
have_class_library ()
{
	return g_file_test (CLASS_LIBRARY_DIR "/mscorlib.dll", G_FILE_TEST_EXISTS);
}

} // namespace test
} // namespace mono
