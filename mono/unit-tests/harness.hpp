/*
 * Shared scaffolding for the unit tests that need a live runtime.
 *
 * No mono headers here: test-mono-callspec.cpp reaches the runtime with
 * MONO_INSIDE_RUNTIME set and its own idea of MONO_RT_EXTERNAL_ONLY, and this
 * has to be includable before any of that is decided.
 */

#ifndef MONO_UNIT_TESTS_HARNESS_HPP
#define MONO_UNIT_TESTS_HARNESS_HPP

namespace mono {
namespace test {

/// Start the runtime, once per process. Safe to call repeatedly.
void init_runtime ();

} // namespace test
} // namespace mono

#endif
