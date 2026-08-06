#include "config.h"

#include <glib.h>
#include <mono/metadata/assembly.h>
#include <mono/mini/jit.h>

#include "harness.hpp"

namespace mono {
namespace test {

/*
 * Where the class libraries are, relative to the working directory the tests are
 * given, which mono/unit-tests/CMakeLists.txt pins for the same reason
 * callspec.exe is opened by bare name.
 */
#define CLASS_LIBRARY_DIR "../../mcs/class/lib/net_4_x"

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
