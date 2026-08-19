/*
 * Tests for the code memory JIT'd objects are linked into.
 *
 * Protections are read out of /proc/self/maps rather than out of the arena's
 * own bookkeeping, so a mapping that is not what it claims cannot pass by
 * agreeing with itself. The property nothing else here would catch is reach:
 * every fixup inside an object, and the .eh_frame delta back to the code, is a
 * PCRel32 that JITLink hard-errors on rather than stubbing, so two reservations
 * that end up more than 2GB apart break a compile a long way from here.
 */

#include "jitlink-memory.hpp"

#include "harness.hpp"

#include <gtest/gtest.h>

#include <llvm/Support/Error.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace llvm;

namespace mono {
namespace test {
namespace {

/*
 * An arena reaches mono's code manager, which wants the runtime's counters and
 * profiler up - so these need a runtime, which needs a class library to boot on.
 */
class CodeMemory : public ::testing::Test {
public:
	static void SetUpTestSuite ()
	{
		MONO_SKIP_WITHOUT_CORPUS ();
		init_runtime ();
	}

protected:
	CodeArena arena;

	char *must_reserve (size_t size, size_t align)
	{
		Expected<char *> mem = arena.reserve (size, align);
		EXPECT_TRUE (bool (mem)) << toString (mem.takeError ());
		return mem ? *mem : nullptr;
	}
};

/// What /proc/self/maps says about addr, as the four rwxp characters.
std::string
mapping_perms (const void *addr)
{
	FILE *maps = fopen ("/proc/self/maps", "r");
	uintptr_t want = reinterpret_cast<uintptr_t> (addr);
	char line[512];
	std::string perms;

	if (!maps)
		return perms;

	while (fgets (line, sizeof (line), maps)) {
		unsigned long long start = 0, end = 0;
		char got[8] = { 0 };

		if (sscanf (line, "%llx-%llx %4s", &start, &end, got) != 3)
			continue;
		if (want >= start && want < end) {
			perms = got;
			break;
		}
	}

	fclose (maps);
	return perms;
}

bool
is_writable (const void *addr)
{
	std::string perms = mapping_perms (addr);
	return perms.size () >= 2 && perms[1] == 'w';
}

bool
is_executable (const void *addr)
{
	std::string perms = mapping_perms (addr);
	return perms.size () >= 3 && perms[2] == 'x';
}

TEST_F (CodeMemory, ReservationsAreDisjoint)
{
	char *a = must_reserve (64, 16);
	char *b = must_reserve (64, 16);
	char *c = must_reserve (200, 16);

	ASSERT_NE (a, nullptr);
	ASSERT_NE (b, nullptr);
	ASSERT_NE (c, nullptr);

	EXPECT_TRUE (a + 64 <= b || b + 64 <= a);
	EXPECT_TRUE (a + 64 <= c || c + 200 <= a);
	EXPECT_TRUE (b + 64 <= c || c + 200 <= b);
}

TEST_F (CodeMemory, ReservationsAreWritableAndExecutable)
{
	char *a = must_reserve (128, 16);

	ASSERT_NE (a, nullptr);
	EXPECT_TRUE (is_writable (a));
	EXPECT_TRUE (is_executable (a));

	/* Writable means writable, not merely marked so. */
	memset (a, 0xcc, 128);
}

/*
 * The code manager promises 16, so anything wider is served by over-allocating.
 * A constant pool of AVX vectors is where this comes up.
 */
TEST_F (CodeMemory, AlignmentAboveWhatTheCodeManagerPromises)
{
	must_reserve (7, 1);

	for (size_t align : { size_t (16), size_t (32), size_t (64), size_t (256) }) {
		char *a = must_reserve (32, align);

		ASSERT_NE (a, nullptr);
		EXPECT_EQ (reinterpret_cast<uintptr_t> (a) % align, 0u)
			<< "at alignment " << align;
	}
}

/*
 * Every fixup a compiled object makes to itself is a PCRel32 with no stub to
 * fall back on, so two allocations have to stay inside what one can encode. The
 * code manager gets that by mapping its chunks MAP_32BIT.
 */
TEST_F (CodeMemory, ReservationsStayWithinPcrel32Reach)
{
	constexpr uintptr_t reach = uintptr_t (2) * 1024 * 1024 * 1024;
	std::vector<char *> mem;

	for (int i = 0; i < 64; i++) {
		char *a = must_reserve (4096, 16);

		ASSERT_NE (a, nullptr);
		mem.push_back (a);
	}

	for (char *a : mem)
		EXPECT_LT (reinterpret_cast<uintptr_t> (a) + 4096, reach);
}

TEST_F (CodeMemory, ConcurrentReservationsDoNotOverlap)
{
	constexpr int threads_count = 8;
	constexpr int per_thread = 500;
	constexpr size_t size = 48;

	std::vector<std::thread> threads;
	std::vector<char *> mem;
	std::mutex mutex;
	std::atomic<int> failures { 0 };

	for (int t = 0; t < threads_count; t++)
		threads.emplace_back ([&] {
			std::vector<char *> mine;

			for (int i = 0; i < per_thread; i++) {
				Expected<char *> a = arena.reserve (size, 16);

				if (!a) {
					consumeError (a.takeError ());
					failures++;
					return;
				}
				memset (*a, 0x90, size);
				mine.push_back (*a);
			}

			std::lock_guard<std::mutex> lock (mutex);
			mem.insert (mem.end (), mine.begin (), mine.end ());
		});

	for (std::thread &t : threads)
		t.join ();

	EXPECT_EQ (failures.load (), 0);
	ASSERT_EQ (mem.size (), size_t (threads_count * per_thread));

	std::sort (mem.begin (), mem.end ());
	for (size_t i = 1; i < mem.size (); i++)
		ASSERT_GE (mem[i] - mem[i - 1], ptrdiff_t (size))
			<< "reservations " << (i - 1) << " and " << i << " overlap";
}

} // namespace
} // namespace test
} // namespace mono
