/*
 * Regression test for a race in llvm::ValueProfData::serializeFrom (), which
 * writes through a file-scope closure with no lock of its own
 * (llvm/lib/ProfileData/InstrProf.cpp). mono::build_profile () is the only
 * caller of InstrProfWriter::write* () in this tree, and the compile queue
 * calls it from several worker threads at once with no synchronization of
 * its own, so two overlapping calls raced on that closure and crashed.
 *
 * Pure LLVM, like jit.cpp's own build_profile (): no runtime, no JIT, no
 * corpus. Each thread hands build_profile () a profile layout backed by its
 * own local counters, over and over, so many calls overlap.
 */

#include "jit.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

using namespace llvm;

namespace mono {
namespace test {
namespace {

constexpr unsigned kIterations = 3000;
constexpr unsigned kFunctionsPerCall = 8;
constexpr unsigned kCountersPerFunction = 32;

/// One thread's share of the stress: build a layout of its own on every
/// iteration and hand it to build_profile ().
void hammer (unsigned thread_index, std::atomic<bool> &saw_empty_buffer)
{
	for (unsigned iter = 0; iter < kIterations; iter++) {
		std::vector<std::vector<uint64_t>> backing (kFunctionsPerCall);
		std::vector<ProfileCounters> layout (kFunctionsPerCall);

		for (unsigned f = 0; f < kFunctionsPerCall; f++) {
			backing[f].resize (kCountersPerFunction);
			for (unsigned c = 0; c < kCountersPerFunction; c++)
				backing[f][c] = thread_index * 1000000u + iter * 100u + c;

			layout[f].function = "fn" + std::to_string (f);
			layout[f].name = "race_fn_" + std::to_string (thread_index) + "_"
				+ std::to_string (iter) + "_" + std::to_string (f);
			layout[f].hash = 0x1000 + thread_index * 1000u + iter * 10u + f;
			layout[f].counters = backing[f].data ();
			layout[f].count = kCountersPerFunction;
		}

		std::vector<uint8_t> written = build_profile (layout);

		if (written.empty ())
			saw_empty_buffer.store (true, std::memory_order_relaxed);
	}
}

} // namespace

/*
 * Many threads calling build_profile () at once, with no serialization of
 * their own. Before the mutex build_profile () now takes, this crashes the
 * process (SIGSEGV or SIGABRT) rather than failing an assertion, because the
 * race is in ValueProfData::serializeFrom ()'s own shared state and not in
 * anything this test can catch with EXPECT. Passing is surviving every
 * iteration on every thread; a crash takes the whole binary down, which
 * ctest reports as this test failing.
 */
TEST (InstrProfWriterRace, ConcurrentBuildProfileDoesNotCrash)
{
	unsigned threads_n = std::max (4u, std::thread::hardware_concurrency ());
	std::atomic<bool> saw_empty_buffer { false };
	std::vector<std::thread> threads;

	threads.reserve (threads_n);
	for (unsigned t = 0; t < threads_n; t++)
		threads.emplace_back (hammer, t, std::ref (saw_empty_buffer));

	for (std::thread &th : threads)
		th.join ();

	EXPECT_FALSE (saw_empty_buffer.load ());
}

} // namespace test
} // namespace mono
