#include "runtime/engine.hpp"

#include <gtest/gtest.h>

#include <stdlib.h>

using namespace mono;

TEST (Engine, DefaultsToTheNewEngine)
{
	unsetenv ("MONO_LLVM_JIT_ENGINE");
	EXPECT_EQ (selected_engine (), EngineKind::backend);
}

/*
 * The two engines keep separate state for the same method and would both write
 * into mono's own jit-info table, and whichever starts second silently loses
 * every --llvm-opt the first one already applied. Claiming is what turns that
 * into an abort, so it is worth a case of its own - the alternative is noticing
 * years later that a flag stopped working.
 */
TEST (Engine, ClaimingBothEnginesDies)
{
	claim_engine (EngineKind::backend);
	claim_engine (EngineKind::backend);

	EXPECT_DEATH (claim_engine (EngineKind::legacy), "both llvm engines");
}
