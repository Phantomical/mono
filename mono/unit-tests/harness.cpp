#include "config.h"

#include <mono/metadata/assembly.h>
#include <mono/mini/jit.h>

#include "harness.hpp"

namespace mono {
namespace test {

/*
 * Every suite in the binary shares this one runtime, so which of them gets here
 * first cannot matter -- and it varies, since ctest runs each case on its own
 * and --gtest_shuffle reorders the rest.  The assemblies path is relative to the
 * working directory the tests are given, which mono/unit-tests/CMakeLists.txt
 * pins for the same reason callspec.exe is opened by bare name.
 */
void
init_runtime ()
{
	static bool started = false;

	if (started)
		return;
	started = true;

	//FIXME This is a hack due to embedding simply not working from the tree
	mono_set_assemblies_path ("../../mcs/class/lib/net_4_x");
	mono_jit_init_version_for_test_only ("mono-unit-tests", "v4.0.30319");
}

} // namespace test
} // namespace mono
