/*
 * Shared scaffolding for the unit tests that need a live runtime and the class
 * libraries it runs on.
 *
 * No mono headers here: test-mono-callspec.cpp reaches the runtime with
 * MONO_INSIDE_RUNTIME set and its own idea of MONO_RT_EXTERNAL_ONLY, and this
 * has to be includable before any of that is decided.
 */

#ifndef MONO_UNIT_TESTS_HARNESS_HPP
#define MONO_UNIT_TESTS_HARNESS_HPP

#include <gtest/gtest.h>

namespace mono {
namespace test {

/// Start the runtime, once per process. Safe to call repeatedly.
void init_runtime ();

/// Whether this build has the class libraries the runtime starts on.
bool have_class_library ();

} // namespace test
} // namespace mono

/*
 * Skip the case unless the class libraries are there.  A build configured with
 * -DMONO_ENABLE_MCS_BUILD=OFF has a runtime and nothing managed, so starting one
 * dies on the missing mscorlib before any body runs -- and a case that cannot run
 * has to say so as a skip rather than as a failure.  Good in a test body, in
 * SetUp (), and in SetUpTestSuite (), which is where the fixtures that bring the
 * runtime up need it.
 */
#define MONO_SKIP_WITHOUT_CLASS_LIBRARY()				\
	do {								\
		if (!mono::test::have_class_library ())			\
			GTEST_SKIP () << "no class libraries in this build"; \
	} while (0)

#endif
