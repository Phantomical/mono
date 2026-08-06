/*
 * test-embed-invoke.cpp: Unit test for embed mono.
 *
 * The body is samples/embed/test-invoke.c itself, so the sample an embedder is
 * pointed at is the thing under test.  _TESTCASE_ makes it define an entry point
 * instead of a main ().
 */

#define _TESTCASE_
#include <mono/mini/jit.h>
#include <embed/test-invoke.c>

#include <gtest/gtest.h>

/*
 * Only one case here, and there could not be a second: the sample runs a domain
 * to completion and tears it down, and a runtime does not start again after
 * that.
 */
TEST (EmbedInvoke, RunSampleAssembly)
{
	EXPECT_EQ (0, test_mono_embed_invoke_main ());
}
