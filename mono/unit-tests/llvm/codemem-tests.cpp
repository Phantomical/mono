/*
 * Tests for the slab allocator JIT'd code is carved out of.
 *
 * The interesting part is the page-completion accounting: pages have to be
 * writable for exactly as long as somebody is still writing to them, which two
 * allocations sharing a page and one allocation spanning several pages both
 * pull on from different directions. Protections are checked against
 * /proc/self/maps rather than against the allocator's own bookkeeping, so a
 * missing mprotect cannot pass by agreeing with itself.
 */

#include "codemem.hpp"

#include <gtest/gtest.h>

#include <llvm/Support/Error.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace llvm;

namespace mono {
namespace test {
namespace {

std::shared_ptr<CodeSlabs>
make_slabs ()
{
	Expected<std::shared_ptr<CodeSlabs>> slabs = CodeSlabs::create ();
	EXPECT_TRUE (bool (slabs)) << toString (slabs.takeError ());
	return slabs ? *slabs : nullptr;
}

CodeSlabs::Alloc
must_allocate (CodeSlabs &slabs, size_t size, size_t align)
{
	Expected<CodeSlabs::Alloc> alloc = slabs.allocate (size, align);
	EXPECT_TRUE (bool (alloc)) << toString (alloc.takeError ());
	return alloc ? *alloc : CodeSlabs::Alloc {};
}

/// What /proc/self/maps says about ADDR, as the four rwxp characters.
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

bool
is_mapped (const void *addr)
{
	return !mapping_perms (addr).empty ();
}

TEST (CodeSlabs, BumpAllocationIsOrderedAndDisjoint)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	CodeSlabs::Alloc a = must_allocate (*slabs, 64, 16);
	CodeSlabs::Alloc b = must_allocate (*slabs, 64, 16);
	CodeSlabs::Alloc c = must_allocate (*slabs, 200, 16);

	EXPECT_GE (b.base, a.base + a.size);
	EXPECT_GE (c.base, b.base + b.size);
	EXPECT_EQ (slabs->slab_count (), 1u);
	EXPECT_EQ (slabs->live_bytes (), 64u + 64u + 200u);
}

TEST (CodeSlabs, AllocationsAreWritableAndExecutable)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	CodeSlabs::Alloc a = must_allocate (*slabs, 128, 16);

	EXPECT_TRUE (is_writable (a.base));
	EXPECT_TRUE (is_executable (a.base));

	/* Writable means writable, not merely marked so. */
	memset (a.base, 0xcc, a.size);

	ASSERT_FALSE (bool (slabs->finish (a)));
	EXPECT_FALSE (is_writable (a.base));
	EXPECT_TRUE (is_executable (a.base));
}

TEST (CodeSlabs, AlignmentIsHonouredAboveAPage)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	must_allocate (*slabs, 7, 1);

	size_t align = slabs->page_size () * 4;
	CodeSlabs::Alloc a = must_allocate (*slabs, 32, align);

	EXPECT_EQ (reinterpret_cast<uintptr_t> (a.base) % align, 0u);
}

TEST (CodeSlabs, ASharedPageSealsOnlyWhenBothAllocationsFinish)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	CodeSlabs::Alloc a = must_allocate (*slabs, 64, 16);
	CodeSlabs::Alloc b = must_allocate (*slabs, 64, 16);

	ASSERT_EQ (reinterpret_cast<uintptr_t> (a.base) / slabs->page_size (),
	           reinterpret_cast<uintptr_t> (b.base) / slabs->page_size ());

	ASSERT_FALSE (bool (slabs->finish (a)));
	EXPECT_TRUE (is_writable (a.base)) << "sealed while b was still writing";

	ASSERT_FALSE (bool (slabs->finish (b)));
	EXPECT_FALSE (is_writable (a.base));
}

TEST (CodeSlabs, SharedPageSealsWhicheverOrderTheyFinishIn)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	CodeSlabs::Alloc a = must_allocate (*slabs, 64, 16);
	CodeSlabs::Alloc b = must_allocate (*slabs, 64, 16);

	ASSERT_FALSE (bool (slabs->finish (b)));
	EXPECT_TRUE (is_writable (a.base));

	ASSERT_FALSE (bool (slabs->finish (a)));
	EXPECT_FALSE (is_writable (a.base));
}

TEST (CodeSlabs, AMultiPageAllocationSealsAllButItsSharedTail)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	size_t page = slabs->page_size ();

	/* Starts a page in, so `big` covers three pages and ends mid-page. */
	CodeSlabs::Alloc head = must_allocate (*slabs, page, page);
	CodeSlabs::Alloc big = must_allocate (*slabs, page * 2 + 16, 16);
	CodeSlabs::Alloc tail = must_allocate (*slabs, 16, 16);

	ASSERT_FALSE (bool (slabs->finish (head)));

	memset (big.base, 0x90, big.size);
	ASSERT_FALSE (bool (slabs->finish (big)));

	EXPECT_FALSE (is_writable (big.base));
	EXPECT_FALSE (is_writable (big.base + page));
	EXPECT_TRUE (is_writable (tail.base)) << "tail page sealed under its writer";

	ASSERT_FALSE (bool (slabs->finish (tail)));
	EXPECT_FALSE (is_writable (tail.base));
	EXPECT_TRUE (is_executable (tail.base));
}

TEST (CodeSlabs, ASealedPageReopensForTheNextAllocation)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	CodeSlabs::Alloc a = must_allocate (*slabs, 64, 16);
	ASSERT_FALSE (bool (slabs->finish (a)));
	ASSERT_FALSE (is_writable (a.base));

	CodeSlabs::Alloc b = must_allocate (*slabs, 64, 16);
	ASSERT_EQ (reinterpret_cast<uintptr_t> (a.base) / slabs->page_size (),
	           reinterpret_cast<uintptr_t> (b.base) / slabs->page_size ());

	EXPECT_TRUE (is_writable (a.base));
	memset (b.base, 0xcc, b.size);

	ASSERT_FALSE (bool (slabs->finish (b)));
	EXPECT_FALSE (is_writable (a.base));
}

TEST (CodeSlabs, AbandonSealsAndRewindsTheBump)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	CodeSlabs::Alloc a = must_allocate (*slabs, 64, 16);
	ASSERT_FALSE (bool (slabs->finish (a)));

	CodeSlabs::Alloc doomed = must_allocate (*slabs, 64, 16);
	ASSERT_FALSE (bool (slabs->abandon (doomed)));
	EXPECT_FALSE (is_writable (a.base)) << "abandon left the page open";
	EXPECT_EQ (slabs->live_bytes (), 64u);

	CodeSlabs::Alloc next = must_allocate (*slabs, 64, 16);
	EXPECT_EQ (next.base, doomed.base) << "abandoned bytes were not reused";
}

TEST (CodeSlabs, AbandonInTheMiddleLeavesTheBytesReusable)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	CodeSlabs::Alloc doomed = must_allocate (*slabs, 64, 16);
	CodeSlabs::Alloc after = must_allocate (*slabs, 64, 16);

	ASSERT_FALSE (bool (slabs->abandon (doomed)));
	ASSERT_FALSE (bool (slabs->finish (after)));

	CodeSlabs::Alloc next = must_allocate (*slabs, 64, 16);
	EXPECT_EQ (next.base, doomed.base);
}

TEST (CodeSlabs, ReleasedRangesAreReusedAndCoalesced)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	CodeSlabs::Alloc a = must_allocate (*slabs, 64, 16);
	CodeSlabs::Alloc b = must_allocate (*slabs, 64, 16);
	CodeSlabs::Alloc c = must_allocate (*slabs, 64, 16);

	ASSERT_FALSE (bool (slabs->finish (a)));
	ASSERT_FALSE (bool (slabs->finish (b)));
	ASSERT_FALSE (bool (slabs->finish (c)));

	slabs->release (a);
	slabs->release (b);
	EXPECT_EQ (slabs->live_bytes (), 64u);

	/* Only a coalesced free range can satisfy this out of the two 64s. */
	CodeSlabs::Alloc wide = must_allocate (*slabs, 128, 16);
	EXPECT_EQ (wide.base, a.base);

	ASSERT_FALSE (bool (slabs->finish (wide)));
	slabs->release (wide);
	slabs->release (c);
	EXPECT_EQ (slabs->live_bytes (), 0u);
}

TEST (CodeSlabs, ReuseGoesThroughTheSameSealingPath)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	CodeSlabs::Alloc a = must_allocate (*slabs, 64, 16);
	ASSERT_FALSE (bool (slabs->finish (a)));
	slabs->release (a);
	ASSERT_FALSE (is_writable (a.base));

	CodeSlabs::Alloc b = must_allocate (*slabs, 64, 16);
	ASSERT_EQ (b.base, a.base);
	EXPECT_TRUE (is_writable (b.base));

	memset (b.base, 0xcc, b.size);
	ASSERT_FALSE (bool (slabs->finish (b)));
	EXPECT_FALSE (is_writable (b.base));
}

TEST (CodeSlabs, CommittedBytesTrackPagesRatherThanReservation)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	size_t page = slabs->page_size ();

	EXPECT_EQ (slabs->committed_bytes (), 0u);

	CodeSlabs::Alloc a = must_allocate (*slabs, 16, 16);
	EXPECT_EQ (slabs->committed_bytes (), page);
	ASSERT_FALSE (bool (slabs->finish (a)));

	/* A thousand small methods still cost a handful of pages, which is the
	 * whole point of the exercise. */
	for (int i = 0; i < 1000; i++) {
		CodeSlabs::Alloc n = must_allocate (*slabs, 16, 16);
		ASSERT_FALSE (bool (slabs->finish (n)));
	}
	EXPECT_LE (slabs->committed_bytes (), 8 * page);
}

/// Resident bytes of this process, from /proc/self/statm.
size_t
resident_bytes ()
{
	FILE *f = fopen ("/proc/self/statm", "r");
	unsigned long total = 0, rss = 0;

	if (!f)
		return 0;
	if (fscanf (f, "%lu %lu", &total, &rss) != 2)
		rss = 0;
	fclose (f);
	return size_t (rss) * 4096;
}

/*
 * The failure this guards against is a domain that mints and drops dynamic
 * methods holding its high-water mark for as long as it lives. Reusing the
 * bytes is not enough on its own - a burst that frees everything afterwards
 * would stay resident - so what has to come back down is the kernel's idea of
 * how much memory this process is using, not just the allocator's.
 */
TEST (CodeSlabs, FreedPagesGoBackToTheKernel)
{
	setenv ("MONO_LLVM_SLAB_SIZE", "128M", 1);
	auto slabs = make_slabs ();
	unsetenv ("MONO_LLVM_SLAB_SIZE");
	ASSERT_TRUE (slabs != nullptr);

	size_t chunk = 64 * 1024;
	size_t count = 512; /* 32MB, all live at once */
	std::vector<CodeSlabs::Alloc> live;
	size_t rss_idle = resident_bytes ();

	for (size_t i = 0; i < count; i++) {
		CodeSlabs::Alloc a = must_allocate (*slabs, chunk, 16);

		memset (a.base, 0x90, a.size);
		ASSERT_FALSE (bool (slabs->finish (a)));
		live.push_back (a);
	}

	size_t rss_peak = resident_bytes ();
	EXPECT_GE (rss_peak - rss_idle, count * chunk / 2)
		<< "the burst never became resident, so the drop proves nothing";
	EXPECT_GE (slabs->committed_bytes (), count * chunk);

	for (CodeSlabs::Alloc &a : live)
		slabs->release (a);

	EXPECT_EQ (slabs->live_bytes (), 0u);
	EXPECT_LE (slabs->committed_bytes (), 2 * slabs->page_size ())
		<< "committed bytes did not come back down";

	size_t rss_after = resident_bytes ();
	EXPECT_LT (rss_after, rss_peak - count * chunk / 2)
		<< "the pages were never handed back to the kernel";
}

TEST (CodeSlabs, TheWritableRegionComesFromTheTopAndNeverSeals)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	CodeSlabs::Alloc code = must_allocate (*slabs, 64, 16);
	Expected<CodeSlabs::Alloc> stubs = slabs->allocate_writable (4096, 16);
	ASSERT_TRUE (bool (stubs)) << toString (stubs.takeError ());

	EXPECT_GT (stubs->base, code.base);
	EXPECT_TRUE (is_writable (stubs->base));
	EXPECT_TRUE (is_executable (stubs->base));

	Expected<CodeSlabs::Alloc> more = slabs->allocate_writable (4096, 16);
	ASSERT_TRUE (bool (more)) << toString (more.takeError ());
	EXPECT_LT (more->base, stubs->base) << "writable region did not grow down";

	/* Sealing the code region has nothing to say about the writable one. */
	ASSERT_FALSE (bool (slabs->finish (code)));
	EXPECT_TRUE (is_writable (stubs->base));
	EXPECT_TRUE (is_writable (more->base));

	slabs->release_writable (*stubs);
	Expected<CodeSlabs::Alloc> again = slabs->allocate_writable (4096, 16);
	ASSERT_TRUE (bool (again)) << toString (again.takeError ());
	EXPECT_EQ (again->base, stubs->base) << "released bytes were not reused";
}

TEST (CodeSlabs, AnObjectsCodeAndDataShareASlabButNotAPage)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	Expected<CodeSlabs::Object> obj = slabs->allocate_object (300, 16, 8, 8);
	ASSERT_TRUE (bool (obj)) << toString (obj.takeError ());

	EXPECT_EQ (obj->code.slab, obj->data.slab);
	EXPECT_GT (obj->data.base, obj->code.base);

	/* The PCRel32 edge from the code to its own mutable data has no stub to
	 * fall back on, so a slab has to stay well inside that reach. */
	EXPECT_LT (size_t (obj->data.base - obj->code.base), size_t (2) << 30);

	EXPECT_TRUE (is_writable (obj->code.base));
	EXPECT_TRUE (is_writable (obj->data.base));

	memset (obj->code.base, 0x90, obj->code.size);
	memset (obj->data.base, 0, obj->data.size);
	ASSERT_FALSE (bool (slabs->finish (*obj)));

	EXPECT_FALSE (is_writable (obj->code.base)) << "code did not seal";
	EXPECT_TRUE (is_writable (obj->data.base)) << "data must stay writable";

	/* What the running program actually does to a cast cache. */
	*reinterpret_cast<void **> (obj->data.base) = obj->code.base;

	slabs->release (*obj);
	EXPECT_EQ (slabs->live_bytes (), 0u);
}

TEST (CodeSlabs, AnObjectWithNoDataIsStillFine)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	Expected<CodeSlabs::Object> obj = slabs->allocate_object (128, 16, 0, 1);
	ASSERT_TRUE (bool (obj)) << toString (obj.takeError ());

	EXPECT_NE (obj->code.base, nullptr);
	EXPECT_EQ (obj->data.base, nullptr);

	ASSERT_FALSE (bool (slabs->finish (*obj)));
	slabs->release (*obj);
	EXPECT_EQ (slabs->live_bytes (), 0u);
}

/*
 * The guard page. A slab is exactly as large as a 32-bit displacement reaches,
 * so the last few bytes of it are not usable: a fixup field sitting there is
 * four bytes wide and its addend can be another four bytes negative. Neither
 * region may hand out anything in the top page.
 */
TEST (CodeSlabs, TheTopOfASlabIsNeverHandedOut)
{
	setenv ("MONO_LLVM_SLAB_SIZE", "16M", 1);
	auto slabs = make_slabs ();
	unsetenv ("MONO_LLVM_SLAB_SIZE");
	ASSERT_TRUE (slabs != nullptr);

	size_t page = slabs->page_size ();
	/* The code region bumps up from offset zero, so the first allocation out
	 * of a fresh slab names the slab's base. */
	char *base = must_allocate (*slabs, 16, 16).base;
	char *guard = base + 16 * 1024 * 1024 - page;

	/* The writable region bumps down from the guard page, not from the top. */
	Expected<CodeSlabs::Alloc> first = slabs->allocate_writable (1, 1);
	ASSERT_TRUE (bool (first)) << toString (first.takeError ());
	EXPECT_EQ (first->base + first->size, guard);

	for (size_t size : { size_t (1), size_t (64), page * 2 })
		for (size_t align : { size_t (1), size_t (16), page }) {
			Expected<CodeSlabs::Alloc> a =
				slabs->allocate_writable (size, align);

			ASSERT_TRUE (bool (a)) << toString (a.takeError ());
			EXPECT_LE (a->base + a->size, guard)
				<< "a writable allocation reached into the guard page";
		}
}

TEST (CodeSlabs, TheCodeRegionStopsBelowTheGuardPage)
{
	setenv ("MONO_LLVM_SLAB_SIZE", "16M", 1);
	auto slabs = make_slabs ();
	unsetenv ("MONO_LLVM_SLAB_SIZE");
	ASSERT_TRUE (slabs != nullptr);

	size_t page = slabs->page_size ();
	Expected<CodeSlabs::Alloc> whole =
		slabs->allocate (16 * 1024 * 1024 - page, 16);

	ASSERT_TRUE (bool (whole)) << toString (whole.takeError ());
	ASSERT_FALSE (bool (slabs->finish (*whole)));
	ASSERT_EQ (slabs->slab_count (), 1u);

	/* Everything a slab has is now spoken for, so the next byte has to come
	 * out of a new reservation rather than out of the guard page. */
	Expected<CodeSlabs::Alloc> next = slabs->allocate (16, 16);
	ASSERT_TRUE (bool (next)) << toString (next.takeError ());
	EXPECT_EQ (slabs->slab_count (), 2u) << "code climbed into the guard page";
}

/*
 * A slab is address space and nothing else until something is written into it,
 * which is what makes a reservation this large affordable in the first place.
 */
TEST (CodeSlabs, TheDefaultSlabIsTwoGigabytesAndCostsNothingResident)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	size_t rss_before = resident_bytes ();
	CodeSlabs::Alloc a = must_allocate (*slabs, 64, 16);

	ASSERT_FALSE (bool (slabs->finish (a)));

	const char *last_page = a.base + (size_t (2) << 30) - slabs->page_size ();

	ASSERT_TRUE (is_mapped (last_page)) << "the slab is not 2GB";
	EXPECT_EQ (mapping_perms (last_page), "---p");
	EXPECT_EQ (slabs->committed_bytes (), slabs->page_size ());
	EXPECT_LT (resident_bytes (), rss_before + 8 * 1024 * 1024)
		<< "reserving 2GB became resident";
}

TEST (CodeSlabs, TheReservationItselfIsNotMapped)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	CodeSlabs::Alloc a = must_allocate (*slabs, 64, 16);
	ASSERT_FALSE (bool (slabs->finish (a)));

	/* PROT_NONE shows as ---p, so the untouched remainder is neither readable
	 * nor writable nor executable however far past the bump we look. */
	const char *far = a.base + 64 * 1024 * 1024;
	ASSERT_TRUE (is_mapped (far));
	EXPECT_EQ (mapping_perms (far), "---p");
}

TEST (CodeSlabs, OverflowingASlabAllocatesAnother)
{
	setenv ("MONO_LLVM_SLAB_SIZE", "16M", 1);
	auto slabs = make_slabs ();
	unsetenv ("MONO_LLVM_SLAB_SIZE");
	ASSERT_TRUE (slabs != nullptr);

	size_t chunk = 1024 * 1024;
	std::vector<CodeSlabs::Alloc> allocs;

	for (int i = 0; i < 20; i++) {
		CodeSlabs::Alloc a = must_allocate (*slabs, chunk, 16);
		ASSERT_FALSE (bool (slabs->finish (a)));
		allocs.push_back (a);
	}

	EXPECT_GT (slabs->slab_count (), 1u);
	EXPECT_EQ (slabs->live_bytes (), 20 * chunk);

	/* Nothing straddles a slab: every allocation is one contiguous run inside
	 * whichever reservation it landed in. */
	for (CodeSlabs::Alloc &a : allocs)
		EXPECT_TRUE (is_mapped (a.base + a.size - 1));
}

TEST (CodeSlabs, ConcurrentAllocatorsShareTailPagesSafely)
{
	auto slabs = make_slabs ();
	ASSERT_TRUE (slabs != nullptr);

	std::vector<std::thread> threads;
	std::atomic<int> failures { 0 };

	for (int t = 0; t < 8; t++)
		threads.emplace_back ([&slabs, &failures] {
			for (int i = 0; i < 500; i++) {
				Expected<CodeSlabs::Alloc> a =
					slabs->allocate (48, 16);
				if (!a) {
					consumeError (a.takeError ());
					failures++;
					return;
				}
				memset (a->base, 0x90, a->size);
				if (Error err = slabs->finish (*a)) {
					consumeError (std::move (err));
					failures++;
					return;
				}
			}
		});

	for (std::thread &t : threads)
		t.join ();

	EXPECT_EQ (failures.load (), 0);
	EXPECT_EQ (slabs->live_bytes (), size_t (8 * 500 * 48));
}

} // namespace
} // namespace test
} // namespace mono
