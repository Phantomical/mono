/*
 * test-llvm-engine.c: Unit test for the mono/mini/llvm ORCv2 JIT engine.
 *
 * The heavy lifting (building hand-crafted LLVM modules, JITing them through
 * the real engine path, calling the results and checking them) lives in
 * engine.cpp behind the extern "C" entry mono_llvm_engine_run_selftest(), so
 * this C test stays thin - matching the convention of the other tests here.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full
 * license information.
 */

#include "config.h"

#include <assert.h>

/*
 * Declared in mono/mini/llvm/backend.h, but that header pulls in the full LLVM-C
 * and mini headers; the entry point is a plain extern "C" int(void), so declare
 * it directly to keep this test self-contained.
 */
int mono_llvm_engine_run_selftest (void);

#ifdef __cplusplus
extern "C"
#endif
int
test_llvm_engine_main (void);

int
test_llvm_engine_main (void)
{
#ifdef ENABLE_LLVM
	assert (mono_llvm_engine_run_selftest () == 0);
	return 0;
#else
	/* Nothing to test without the LLVM backend built in. */
	return 0;
#endif
}
