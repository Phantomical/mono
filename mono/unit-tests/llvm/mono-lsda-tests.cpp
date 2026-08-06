/*
 * Tests for the load-time `.mono_lsda` publish/validate core in
 * mono/llvm/mono_lsda.cpp.
 *
 * parse_mono_lsda () decodes the target-neutral `.mono_lsda` section the compiler
 * emits next to the code; build_ex_info () validates those tuples against the IL
 * clause table and joins them into a MonoJitExceptionInfo[]. Everything here
 * drives them with byte buffers and synthetic clause tables, offline - no runtime
 * and no compiled method involved.
 *
 * Expectations are hand-derived from the format, not echoed from the
 * implementation.
 */

#include "config.h"

#include "mono_lsda.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

using mono::MonoFinallyGuard;
using mono::MonoLsdaEntry;

namespace mono {
namespace test {
namespace {

constexpr std::uint32_t MLSD_MAGIC = 0x4d4c5344u; /* 'MLSD', little-endian */

/* Little-endian append helpers for assembling .mono_lsda byte vectors. */
void
put_u16 (std::vector<std::uint8_t> &b, std::uint16_t v)
{
	b.push_back ((std::uint8_t) (v & 0xff));
	b.push_back ((std::uint8_t) ((v >> 8) & 0xff));
}

void
put_u32 (std::vector<std::uint8_t> &b, std::uint32_t v)
{
	b.push_back ((std::uint8_t) (v & 0xff));
	b.push_back ((std::uint8_t) ((v >> 8) & 0xff));
	b.push_back ((std::uint8_t) ((v >> 16) & 0xff));
	b.push_back ((std::uint8_t) ((v >> 24) & 0xff));
}

/*
 * Assemble a .mono_lsda section: header (magic, version, count) then the given
 * entries. MAGIC/VERSION/COUNT are parameters so negative cases can corrupt them
 * independently of the entry payload.
 */
std::vector<std::uint8_t>
make_lsda (std::uint32_t magic, std::uint16_t version, std::uint16_t count,
           const std::vector<MonoLsdaEntry> &entries)
{
	std::vector<std::uint8_t> b;

	put_u32 (b, magic);
	put_u16 (b, version);
	put_u16 (b, count);
	for (const MonoLsdaEntry &e : entries) {
		put_u32 (b, e.try_start_off);
		put_u32 (b, e.try_len);
		put_u32 (b, e.handler_off);
		put_u32 (b, e.clause_index);
		put_u32 (b, e.kind);
	}
	return b;
}

/*
 * The exact bytes a writer emits for the v2 format (self-describing kind
 * column), for a two-catch geometry with a trailing per-entry kind == 0:
 *   44534c4d 02000200   magic 'MLSD', version 2, count 2
 *   01000000 05000000 11000000 07000000 00000000  {try=1, len=5, h=0x11, clause=7, kind=0}
 *   06000000 05000000 0f000000 03000000 00000000  {try=6, len=5, h=0x0f, clause=3, kind=0}
 * 48 bytes = 8 + 2*20. Decoded by hand from the format above.
 */
const std::uint8_t GOLDEN_MLSD [] = {
	0x44, 0x53, 0x4c, 0x4d, 0x02, 0x00, 0x02, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	0x11, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x06, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	0x0f, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};

#if defined (HAVE_SYS_MMAN_H) && defined (HAVE_UNISTD_H) && !defined (HOST_WIN32)
#define MONO_LSDA_HAVE_GUARD_PAGE 1
#endif

#ifdef MONO_LSDA_HAVE_GUARD_PAGE

/*
 * A fault inside the guarded decode means the decoder read past the end of the
 * buffer it was given, which is worth saying out loud - the default report is a
 * bare "child killed by signal 11" a long way from the guard page that caused it.
 */
void
crash_handler (int sig)
{
	static const char message [] =
		"\n*** SIGSEGV/SIGBUS in a guarded .mono_lsda decode: the decoder read PAST"
		"\n*** the end of the buffer it was given (its last byte sits against a"
		"\n*** PROT_NONE guard page).\n";
	ssize_t ignored = write (2, message, sizeof (message) - 1);

	(void) ignored;
	signal (sig, SIG_DFL);
	raise (sig);
}

#endif

} // namespace

/*
 * Decodes every buffer with its last byte flush against a PROT_NONE guard page,
 * so any read one byte past the declared size faults immediately instead of
 * reading adjacent memory and returning a plausible-but-wrong answer. The
 * truncation sweep - decoding every prefix length of a valid section - leans on
 * this: a decoder that forgets a bounds check is caught here, not left to a later
 * heisenbug.
 */
class MonoLsdaParse : public ::testing::Test {
protected:
	void
	SetUp () override
	{
#ifdef MONO_LSDA_HAVE_GUARD_PAGE
		long pagel = sysconf (_SC_PAGESIZE);

		page = pagel > 0 ? (std::size_t) pagel : 4096;
		region = (std::uint8_t *) mmap (nullptr, 2 * page, PROT_READ | PROT_WRITE,
		                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (region == MAP_FAILED)
			GTEST_SKIP () << "cannot reserve a guard page: " << strerror (errno);
		if (mprotect (region + page, page, PROT_NONE) != 0) {
			munmap (region, 2 * page);
			region = (std::uint8_t *) MAP_FAILED;
			GTEST_SKIP () << "cannot protect a guard page: " << strerror (errno);
		}
		signal (SIGSEGV, crash_handler);
		signal (SIGBUS, crash_handler);
#else
		GTEST_SKIP () << "no mmap here, so no guard page to decode against";
#endif
	}

	void
	TearDown () override
	{
#ifdef MONO_LSDA_HAVE_GUARD_PAGE
		if (region != MAP_FAILED)
			munmap (region, 2 * page);
#endif
	}

	/// Decode LEN bytes of DATA with the final byte flush against the guard page.
	bool
	parse (const std::uint8_t *data, std::size_t len, std::vector<MonoLsdaEntry> &out)
	{
#ifdef MONO_LSDA_HAVE_GUARD_PAGE
		std::uint8_t *buf = region + page - len; /* last byte at page-1 */

		EXPECT_LE (len, page);
		if (len)
			memcpy (buf, data, len);
		return mono::parse_mono_lsda (buf, len, out);
#else
		(void) data;
		(void) len;
		(void) out;
		return false;
#endif
	}

	/// Decode DATA and require that it is accepted as exactly WANT.
	void
	expect_parse (const std::uint8_t *data, std::size_t len,
	              const std::vector<MonoLsdaEntry> &want)
	{
		std::vector<MonoLsdaEntry> out;

		ASSERT_TRUE (parse (data, len, out)) << "declined a valid section";
		ASSERT_EQ (out.size (), want.size ());
		for (std::size_t i = 0; i < want.size (); ++i) {
			SCOPED_TRACE (::testing::Message () << "entry " << i);
			EXPECT_EQ (out[i].try_start_off, want[i].try_start_off);
			EXPECT_EQ (out[i].try_len, want[i].try_len);
			EXPECT_EQ (out[i].handler_off, want[i].handler_off);
			EXPECT_EQ (out[i].clause_index, want[i].clause_index);
			EXPECT_EQ (out[i].kind, want[i].kind);
		}
	}

	/// Decode DATA and require that it is declined.
	void
	expect_decline (const std::uint8_t *data, std::size_t len)
	{
		std::vector<MonoLsdaEntry> out;

		EXPECT_FALSE (parse (data, len, out)) << "accepted a section that should decline";
	}

private:
	std::size_t page = 0;
#ifdef MONO_LSDA_HAVE_GUARD_PAGE
	std::uint8_t *region = (std::uint8_t *) MAP_FAILED;
#endif
};

/* The golden vector decodes to its two hand-derived entries (kind 0). */
TEST_F (MonoLsdaParse, GoldenTwoEntry)
{
	expect_parse (GOLDEN_MLSD, sizeof (GOLDEN_MLSD),
	              { { 1, 5, 0x11, 7, 0 }, { 6, 5, 0x0f, 3, 0 } });
}

/* A header-only section (count 0) is well-formed and decodes to nothing. */
TEST_F (MonoLsdaParse, CountZeroHeaderOnly)
{
	std::vector<std::uint8_t> b = make_lsda (MLSD_MAGIC, 2, 0, {});

	expect_parse (b.data (), b.size (), {});
}

/* One-entry section: exactly 8 + 20 bytes. Non-zero kind round-trips verbatim. */
TEST_F (MonoLsdaParse, OneEntry)
{
	std::vector<std::uint8_t> b = make_lsda (MLSD_MAGIC, 2, 1, { { 0x20, 0x08, 0x30, 0, 2 } });

	expect_parse (b.data (), b.size (), { { 0x20, 0x08, 0x30, 0, 2 } });
}

TEST_F (MonoLsdaParse, BadMagicDeclines)
{
	std::vector<std::uint8_t> b = make_lsda (0xdeadbeefu, 2, 1, { { 1, 5, 0x11, 7, 0 } });

	expect_decline (b.data (), b.size ());
}

/*
 * A v1 buffer declines against this v2-only loader. It is a genuine 16-byte-stride
 * v1 record (magic ok, version 1, one 16-byte entry): the loader recognises only
 * version 2, so the older format is refused rather than misread at the wrong
 * stride.
 */
TEST_F (MonoLsdaParse, Version1Declines)
{
	std::vector<std::uint8_t> b;

	put_u32 (b, MLSD_MAGIC);
	put_u16 (b, 1); /* version 1 */
	put_u16 (b, 1); /* count 1 */
	put_u32 (b, 1); put_u32 (b, 5); put_u32 (b, 0x11); put_u32 (b, 7); /* one v1 16B entry */
	expect_decline (b.data (), b.size ());
}

/* Any other unrecognised version declines too. */
TEST_F (MonoLsdaParse, Version3Declines)
{
	std::vector<std::uint8_t> b = make_lsda (MLSD_MAGIC, 3, 1, { { 1, 5, 0x11, 7, 0 } });

	expect_decline (b.data (), b.size ());
}

/* Truncated header: 7 bytes, the count field cut short. */
TEST_F (MonoLsdaParse, TruncatedHeaderDeclines)
{
	std::vector<std::uint8_t> b = make_lsda (MLSD_MAGIC, 2, 0, {});

	b.pop_back ();
	expect_decline (b.data (), b.size ());
}

/*
 * Truncated entry: the header says 2 entries but only one entry's worth of
 * payload is present, so the exact-size check (28 != 8 + 2*20) declines.
 */
TEST_F (MonoLsdaParse, TruncatedEntryDeclines)
{
	std::vector<std::uint8_t> b = make_lsda (MLSD_MAGIC, 2, 2, { { 1, 5, 0x11, 7, 0 } });

	expect_decline (b.data (), b.size ());
}

/*
 * THE EXACT-SIZE / TWO-RECORD DECLINE.
 * Two full method records concatenated: the first header declares count 1
 * (expected size 28) but the buffer is 56 bytes. A longer-than-exact section means
 * the one-method-per-module invariant broke; reading only the first record would
 * misattribute clause geometry, so parse declines.
 */
TEST_F (MonoLsdaParse, TwoRecordOversizeDeclines)
{
	std::vector<std::uint8_t> rec = make_lsda (MLSD_MAGIC, 2, 1, { { 1, 5, 0x11, 7, 0 } });
	std::vector<std::uint8_t> two = rec;

	two.insert (two.end (), rec.begin (), rec.end ());
	expect_decline (two.data (), two.size ());
}

/*
 * Trailing-byte oversize: exactly one valid record plus a single junk byte.
 * 29 != 28 -> decline (a section MUST be exactly its declared extent).
 */
TEST_F (MonoLsdaParse, OneTrailingByteOversizeDeclines)
{
	std::vector<std::uint8_t> b = make_lsda (MLSD_MAGIC, 2, 1, { { 1, 5, 0x11, 7, 0 } });

	b.push_back (0xaa);
	expect_decline (b.data (), b.size ());
}

/* Null pointer and zero length both decline without touching memory. */
TEST_F (MonoLsdaParse, NullAndEmptyDecline)
{
	std::vector<MonoLsdaEntry> out;
	const std::uint8_t nothing [1] = { 0 };

	EXPECT_FALSE (mono::parse_mono_lsda (nullptr, 0, out)) << "accepted null";
	expect_decline (nothing, 0);
}

/*
 * Truncation sweep: every proper prefix of the golden vector must decline and must
 * not read past its guarded end (a forgotten bounds check faults here, not in a
 * later heisenbug).
 */
TEST_F (MonoLsdaParse, TruncationSweepDeclines)
{
	for (std::size_t len = 0; len < sizeof (GOLDEN_MLSD); ++len) {
		std::vector<MonoLsdaEntry> out;

		SCOPED_TRACE (::testing::Message () << "prefix length " << len);
		ASSERT_FALSE (parse (GOLDEN_MLSD, len, out)) << "prefix accepted";
	}
}

/* ------------------------------------------------------------ build cases */

namespace {

/* Sentinel catch_class pointers (never dereferenced; build_ex_info only copies). */
MonoClass * const CC0 = (MonoClass *) (std::uintptr_t) 0xC0FFEE00u;
MonoClass * const CC1 = (MonoClass *) (std::uintptr_t) 0xC0FFEE11u;

/*
 * A real code buffer to join against, so try_start/try_end/handler_start come out
 * as valid pointers that can be checked against BASE + offset.
 */
class LsdaJoinTest : public ::testing::Test {
protected:
	static constexpr std::uint32_t code_len = 0x100;

	std::vector<std::uint8_t> code = std::vector<std::uint8_t> (code_len, 0);
	const std::uint8_t *base = code.data ();

	/// BASE + OFF as the gpointer a published entry carries.
	gpointer at (std::uint32_t off) const { return (gpointer) (base + off); }
};

} // namespace

class MonoLsdaBuild : public LsdaJoinTest {
protected:
	/* Two catch clauses (both CLAUSE_NONE) with distinct catch_class sentinels. */
	void
	SetUp () override
	{
		memset (clauses, 0, sizeof (clauses));
		clauses[0].flags = MONO_EXCEPTION_CLAUSE_NONE;
		clauses[0].data.catch_class = CC0;
		clauses[1].flags = MONO_EXCEPTION_CLAUSE_NONE;
		clauses[1].data.catch_class = CC1;
	}

	void
	expect_build_decline (const std::vector<MonoLsdaEntry> &ents)
	{
		std::vector<MonoJitExceptionInfo> out;

		EXPECT_FALSE (mono::build_ex_info (ents, clauses, 2, base, code_len, out))
			<< "accepted entries that should decline";
	}

	MonoExceptionClause clauses [2];
};

/* Two disjoint entries joining onto the two clauses. */
TEST_F (MonoLsdaBuild, ValidTwoClause)
{
	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x20, 0x40, 0 }, /* [0x10,0x30) -> handler 0x40, clause 0 */
		{ 0x50, 0x10, 0x80, 1 }, /* [0x50,0x60) -> handler 0x80, clause 1 */
	};
	std::vector<MonoJitExceptionInfo> out;

	ASSERT_TRUE (mono::build_ex_info (ents, clauses, 2, base, code_len, out));
	ASSERT_EQ (out.size (), 2u);

	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_NONE);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[0].try_start, at (0x10));
	EXPECT_EQ (out[0].try_end, at (0x30));
	EXPECT_EQ (out[0].handler_start, at (0x40));
	EXPECT_EQ (out[0].data.catch_class, CC0);
	EXPECT_EQ (out[0].exvar_offset, 0);
	EXPECT_EQ (out[0].try_offset, 0);
	EXPECT_EQ (out[0].try_len, 0);
	EXPECT_EQ (out[0].handler_offset, 0);
	EXPECT_EQ (out[0].handler_len, 0);

	EXPECT_EQ (out[1].flags, (guint32) MONO_EXCEPTION_CLAUSE_NONE);
	EXPECT_EQ (out[1].clause_index, 1);
	EXPECT_EQ (out[1].try_start, at (0x50));
	EXPECT_EQ (out[1].try_end, at (0x60));
	EXPECT_EQ (out[1].handler_start, at (0x80));
	EXPECT_EQ (out[1].data.catch_class, CC1);
}

/*
 * Multi-call shape: two disjoint entries sharing ONE clause/handler (a try with
 * two protected calls). Both must publish, same clause_index/handler, different
 * try_start - mono's is_address_protected takes the first PC match.
 */
TEST_F (MonoLsdaBuild, MultiCallSharedClause)
{
	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x08, 0x40, 0 },
		{ 0x30, 0x08, 0x40, 0 },
	};
	std::vector<MonoJitExceptionInfo> out;

	ASSERT_TRUE (mono::build_ex_info (ents, clauses, 2, base, code_len, out));
	ASSERT_EQ (out.size (), 2u);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[1].clause_index, 0);
	EXPECT_EQ (out[0].handler_start, out[1].handler_start);
	EXPECT_NE (out[0].try_start, out[1].try_start);
	EXPECT_EQ (out[0].try_start, at (0x10));
	EXPECT_EQ (out[1].try_start, at (0x30));
}

/* try_start_off == code_len (past the code). */
TEST_F (MonoLsdaBuild, TryStartPastCodeDeclines)
{
	expect_build_decline ({ { code_len, 0x00, 0x40, 0 } });
}

/* try_start_off + try_len past code_len (the 64-bit sum check). */
TEST_F (MonoLsdaBuild, TryEndPastCodeDeclines)
{
	expect_build_decline ({ { code_len - 0x10, 0x20, 0x40, 0 } });
}

/*
 * The 64-bit-sum guard, adversarially: try_start_off + try_len is chosen so a
 * 32-bit-wrapped sum would wrongly stay in range (0xFFFFFFF0 + 0x20 wraps mod 2^32
 * to 0x10, comfortably under code_len), but the true 64-bit sum (0x100000010)
 * correctly exceeds code_len (set to 0xFFFFFFFF for this one call only). This must
 * still decline.
 */
TEST_F (MonoLsdaBuild, TryRange32BitOverflowDeclines)
{
	std::vector<MonoLsdaEntry> ents = { { 0xFFFFFFF0u, 0x20u, 0x40u, 0 } };
	std::vector<MonoJitExceptionInfo> out;

	EXPECT_FALSE (mono::build_ex_info (ents, clauses, 2, base, 0xFFFFFFFFu, out));
}

/* handler_off == code_len (past the code). */
TEST_F (MonoLsdaBuild, HandlerPastCodeDeclines)
{
	expect_build_decline ({ { 0x10, 0x08, code_len, 0 } });
}

/*
 * The vacuous no-EH-method path: no clause table and no entries is a trivial
 * accept (the num_clauses>0-with-no-entries fail-safe does not apply when there IS
 * no clause table), producing an empty published array.
 */
TEST_F (MonoLsdaBuild, NoClausesNoEntriesAccepts)
{
	std::vector<MonoJitExceptionInfo> out;

	EXPECT_TRUE (mono::build_ex_info ({}, nullptr, 0, base, code_len, out));
	EXPECT_TRUE (out.empty ());
}

/*
 * FAULT is admitted - the runtime reads neither handler_end nor exvar_offset for a
 * fault clause. Entry kind FAULT agrees with the join; the published ei carries
 * flags FAULT, the joined geometry, and data.handler_end / exvar_offset both 0.
 */
TEST_F (MonoLsdaBuild, FaultAdmits)
{
	MonoExceptionClause flt [1];
	std::vector<MonoJitExceptionInfo> out;

	memset (flt, 0, sizeof (flt));
	flt[0].flags = MONO_EXCEPTION_CLAUSE_FAULT;

	std::vector<MonoLsdaEntry> ents = { { 0x10, 0x08, 0x40, 0, MONO_EXCEPTION_CLAUSE_FAULT } };

	ASSERT_TRUE (mono::build_ex_info (ents, flt, 1, base, code_len, out));
	ASSERT_EQ (out.size (), 1u);
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_FAULT);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[0].try_start, at (0x10));
	EXPECT_EQ (out[0].try_end, at (0x18));
	EXPECT_EQ (out[0].handler_start, at (0x40));
	EXPECT_EQ (out[0].data.handler_end, nullptr);
	EXPECT_EQ (out[0].exvar_offset, 0);
}

/*
 * FINALLY is admitted once its entry kind agrees with the join. The abort-guard
 * fields data.handler_end and exvar_offset are published as 0 together - the
 * invariant is that they are both real or both 0, and the guard entries a separate
 * case below covers are what supply the real pair.
 */
TEST_F (MonoLsdaBuild, FinallyAdmitsWithGuardFieldsZero)
{
	MonoExceptionClause fin [1];
	std::vector<MonoJitExceptionInfo> out;

	memset (fin, 0, sizeof (fin));
	fin[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY;

	std::vector<MonoLsdaEntry> ents = { { 0x10, 0x08, 0x40, 0, MONO_EXCEPTION_CLAUSE_FINALLY } };

	ASSERT_TRUE (mono::build_ex_info (ents, fin, 1, base, code_len, out));
	ASSERT_EQ (out.size (), 1u);
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[0].try_start, at (0x10));
	EXPECT_EQ (out[0].try_end, at (0x18));
	EXPECT_EQ (out[0].handler_start, at (0x40));
	EXPECT_EQ (out[0].data.handler_end, nullptr);
	EXPECT_EQ (out[0].exvar_offset, 0);
}

/*
 * count == 0 while num_clauses > 0: every protected call in this method's IL
 * clause(s) was optimized to a nounwind call, so nothing survived that could ever
 * reach a handler. Confirmed safe, not uncertain - accept with an empty published
 * array rather than decline.
 */
TEST_F (MonoLsdaBuild, EmptyWhileClausesAccepts)
{
	std::vector<MonoJitExceptionInfo> out;

	EXPECT_TRUE (mono::build_ex_info ({}, clauses, 2, base, code_len, out));
	EXPECT_TRUE (out.empty ());
}

/*
 * An entries set containing ONLY a resume-pad marker for a FINALLY clause is the
 * same confirmed-safe case as an empty entries set, just reached through a
 * different path: the clause's OWN try-body had every protected call optimized
 * away (nothing left for it to contribute as a base entry), but it has an
 * encloser, so its resume-pad invoke still gets emitted unconditionally. Accept
 * with an empty published array, not decline.
 */
TEST_F (MonoLsdaBuild, ResumePadOnlyAccepts)
{
	MonoExceptionClause fin [1];
	std::vector<MonoJitExceptionInfo> out;

	memset (fin, 0, sizeof (fin));
	fin[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY;

	std::vector<MonoLsdaEntry> ents = { { 0x10, 0x08, 0x40, 0, mono::MONO_LSDA_KIND_RESUME_PAD } };

	EXPECT_TRUE (mono::build_ex_info (ents, fin, 1, base, code_len, out));
	EXPECT_TRUE (out.empty ());
}

/*
 * SIBLING CATCHES: try { } catch(A) catch(B) is one landing pad with two TypeIds
 * over ONE invoke range, so the writer emits two entries with the SAME range and
 * DIFFERENT clause_index. Both must publish (equal-or-disjoint invariant) - mono
 * matches the shared PC range for both, then picks the type by catch_class with
 * RDX = clause_index.
 */
TEST_F (MonoLsdaBuild, SiblingSameRange)
{
	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x20, 0x60, 0 }, /* catch A: [0x10,0x30) -> handler 0x60, clause 0 */
		{ 0x10, 0x20, 0x90, 1 }, /* catch B: SAME range     -> handler 0x90, clause 1 */
	};
	std::vector<MonoJitExceptionInfo> out;

	ASSERT_TRUE (mono::build_ex_info (ents, clauses, 2, base, code_len, out));
	ASSERT_EQ (out.size (), 2u);
	EXPECT_EQ (out[0].try_start, at (0x10));
	EXPECT_EQ (out[0].try_end, at (0x30));
	EXPECT_EQ (out[1].try_start, at (0x10));
	EXPECT_EQ (out[1].try_end, at (0x30));
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[1].clause_index, 1);
	EXPECT_EQ (out[0].data.catch_class, CC0);
	EXPECT_EQ (out[1].data.catch_class, CC1);
	EXPECT_EQ (out[0].handler_start, at (0x60));
	EXPECT_EQ (out[1].handler_start, at (0x90));
}

/*
 * SECTION ORDER within one pad's run is authoritative - build_ex_info must NOT
 * re-sort it by clause_index. The order comes from the landing pad's own operand
 * list with the gather undoing LLVM's reversal, so it is already declaration-order
 * for siblings and innermost-first for enclosers.
 *
 * Re-sorting on clause_index would look harmless while every clause belongs to the
 * one method being compiled, since ECMA-335 12.4.2.5 makes ascending index mean
 * innermost-first there. It stops being true the moment a chain spans an inlined
 * body, whose clauses are numbered independently of the caller's - so a
 * clause_index sort key would silently reorder a cross-method nest. Feed a run
 * whose indices DESCEND and assert it publishes exactly as given.
 */
TEST_F (MonoLsdaBuild, SectionOrderIsAuthoritative)
{
	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x20, 0x60, 1 }, /* clause 1 first in the run... */
		{ 0x10, 0x20, 0x60, 0 }, /* ...clause 0 second, one shared pad */
	};
	std::vector<MonoJitExceptionInfo> out;

	ASSERT_TRUE (mono::build_ex_info (ents, clauses, 2, base, code_len, out));
	ASSERT_EQ (out.size (), 2u);
	EXPECT_EQ (out[0].try_start, at (0x10));
	EXPECT_EQ (out[1].try_start, at (0x10));
	/* 0 then 1 here would mean a clause_index re-sort crept back in. */
	EXPECT_EQ (out[0].clause_index, 1);
	EXPECT_EQ (out[1].clause_index, 0);
	EXPECT_EQ (out[0].data.catch_class, CC1);
	EXPECT_EQ (out[1].data.catch_class, CC0);
	EXPECT_EQ (out[0].handler_start, at (0x60));
	EXPECT_EQ (out[1].handler_start, at (0x60));
}

/*
 * SIBLING + MULTI-CALL composed: two identical-range sibling pairs at two DIFFERENT
 * disjoint ranges -> 4 entries -> ACCEPT. Pins that equal-and-disjoint compose
 * (each range hosts a sibling pair; the two ranges are disjoint).
 */
TEST_F (MonoLsdaBuild, SiblingPlusMultiCall)
{
	MonoExceptionClause c4 [4];
	std::vector<MonoJitExceptionInfo> out;

	memset (c4, 0, sizeof (c4));
	for (int k = 0; k < 4; ++k) {
		c4[k].flags = MONO_EXCEPTION_CLAUSE_NONE;
		c4[k].data.catch_class = (MonoClass *) (std::uintptr_t) (0xB00B0000u + (unsigned) k);
	}

	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x10, 0x60, 0 }, /* range R1, clause 0 */
		{ 0x10, 0x10, 0x70, 1 }, /* range R1 (sibling), clause 1 */
		{ 0x40, 0x10, 0x80, 2 }, /* range R2 (disjoint), clause 2 */
		{ 0x40, 0x10, 0x90, 3 }, /* range R2 (sibling), clause 3 */
	};

	ASSERT_TRUE (mono::build_ex_info (ents, c4, 4, base, code_len, out));
	ASSERT_EQ (out.size (), 4u);
	EXPECT_EQ (out[0].try_start, out[1].try_start); /* R1 pair shares range */
	EXPECT_EQ (out[2].try_start, out[3].try_start); /* R2 pair shares range */
	EXPECT_NE (out[0].try_start, out[2].try_start); /* R1 and R2 disjoint */
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[3].clause_index, 3);
}

/*
 * Overlapping invoke ranges (the nesting/ordering sanity): [0x10,0x40) and
 * [0x30,0x60) PARTIALLY overlap (not equal) -> decline (ambiguous first-match,
 * unsupported nesting). Equal-or-disjoint exempts only EXACTLY equal ranges.
 */
TEST_F (MonoLsdaBuild, OverlappingRangesDecline)
{
	expect_build_decline ({ { 0x10, 0x30, 0x40, 0 }, { 0x30, 0x30, 0x80, 1 } });
}

/*
 * STRICT nesting: [0x10,0x40) fully contains [0x20,0x30). Not equal, so it still
 * declines - the missed-nesting attack stays covered.
 */
TEST_F (MonoLsdaBuild, StrictNestingDeclines)
{
	expect_build_decline ({ { 0x10, 0x30, 0x40, 0 }, { 0x20, 0x10, 0x80, 1 } });
}

/*
 * Touching-but-disjoint ranges [0x10,0x20) and [0x20,0x30) must be ACCEPTED
 * (half-open ranges that share an endpoint do not overlap).
 */
TEST_F (MonoLsdaBuild, TouchingDisjointRangesAccept)
{
	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x10, 0x40, 0 },
		{ 0x20, 0x10, 0x80, 1 },
	};
	std::vector<MonoJitExceptionInfo> out;

	ASSERT_TRUE (mono::build_ex_info (ents, clauses, 2, base, code_len, out));
	EXPECT_EQ (out.size (), 2u);
}

/* ------------------------------------------------------ nesting chain cases */

/*
 * How build_ex_info turns a nesting chain into published entries. A landing pad
 * names every clause that covers it - its own sibling group, then its enclosers
 * innermost-first - and `.mono_lsda` carries that list in order, one run per invoke
 * range. build_ex_info publishes the run as it stands, so nesting is encoded by
 * same-range entries plus array order.
 *
 * These cases feed such runs directly and assert the published array: count,
 * per-entry range/handler/flags/clause_index, innermost-first slot order within a
 * range, and equal-or-disjoint still holding.
 *
 * The IL clause table each case passes is read ONLY for flags and catch_class -
 * the try offsets are set to a plausible nest for readability, but nothing derives
 * containment from them. That is the point: an inlined body's IL offsets are
 * meaningless in the caller, and the chain has to come from the pads instead.
 */
class MonoLsdaNesting : public LsdaJoinTest {
protected:
	/* The base entry's native geometry, shared by every nested case below. */
	static constexpr std::uint32_t R_START = 0x10, R_LEN = 0x20, H_OFF = 0x40;
};

/*
 * C# try/catch/finally: inner CATCH(E) nested in outer FINALLY. IL clause0 = inner
 * catch, clause1 = outer finally. The inner catch's pad names both, so the gather
 * hands back a two-entry run over one invoke range - two ei in that order, sharing
 * range and handler_start.
 */
TEST_F (MonoLsdaNesting, TryCatchFinally)
{
	MonoExceptionClause cl [2];
	std::vector<MonoJitExceptionInfo> out;

	memset (cl, 0, sizeof (cl));
	cl[0].flags = MONO_EXCEPTION_CLAUSE_NONE;    /* inner catch */
	cl[0].data.catch_class = CC0;
	cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x05;
	cl[1].flags = MONO_EXCEPTION_CLAUSE_FINALLY; /* outer finally */
	cl[1].try_offset = 0x10; cl[1].try_len = 0x15; cl[1].handler_offset = 0x25; cl[1].handler_len = 0x05;

	std::vector<MonoLsdaEntry> ents = {
		{ R_START, R_LEN, H_OFF, 0, MONO_EXCEPTION_CLAUSE_NONE },
		{ R_START, R_LEN, H_OFF, 1, MONO_EXCEPTION_CLAUSE_FINALLY },
	};

	ASSERT_TRUE (mono::build_ex_info (ents, cl, 2, base, code_len, out));
	ASSERT_EQ (out.size (), 2u);

	/* slot 0: the innermost clause, the catch */
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_NONE);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[0].data.catch_class, CC0);
	EXPECT_EQ (out[0].try_start, at (R_START));
	EXPECT_EQ (out[0].try_end, at (R_START + R_LEN));
	EXPECT_EQ (out[0].handler_start, (gpointer) MINI_ADDR_TO_FTNPTR (base + H_OFF));

	/* slot 1: the enclosing finally, SAME range + SAME handler */
	EXPECT_EQ (out[1].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[1].clause_index, 1);
	EXPECT_EQ (out[1].try_start, out[0].try_start);
	EXPECT_EQ (out[1].try_end, out[0].try_end);
	EXPECT_EQ (out[1].handler_start, out[0].handler_start);
	EXPECT_EQ (out[1].data.handler_end, nullptr);
	EXPECT_EQ (out[1].exvar_offset, 0);
}

/*
 * The mirror, and the sharpest case: inner FINALLY nested in outer CATCH(E). The
 * enclosing entry has to carry the catch's flags NONE + catch_class + clause_index.
 */
TEST_F (MonoLsdaNesting, TryFinallyInTryCatch)
{
	MonoExceptionClause cl [2];
	std::vector<MonoJitExceptionInfo> out;

	memset (cl, 0, sizeof (cl));
	cl[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY; /* inner finally */
	cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x05;
	cl[1].flags = MONO_EXCEPTION_CLAUSE_NONE;    /* outer catch */
	cl[1].data.catch_class = CC1;
	cl[1].try_offset = 0x10; cl[1].try_len = 0x15; cl[1].handler_offset = 0x25; cl[1].handler_len = 0x05;

	std::vector<MonoLsdaEntry> ents = {
		{ R_START, R_LEN, H_OFF, 0, MONO_EXCEPTION_CLAUSE_FINALLY },
		{ R_START, R_LEN, H_OFF, 1, MONO_EXCEPTION_CLAUSE_NONE },
	};

	ASSERT_TRUE (mono::build_ex_info (ents, cl, 2, base, code_len, out));
	ASSERT_EQ (out.size (), 2u);
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[0].try_start, at (R_START));
	EXPECT_EQ (out[0].try_end, at (R_START + R_LEN));
	EXPECT_EQ (out[0].handler_start, (gpointer) MINI_ADDR_TO_FTNPTR (base + H_OFF));
	EXPECT_EQ (out[1].flags, (guint32) MONO_EXCEPTION_CLAUSE_NONE);
	EXPECT_EQ (out[1].clause_index, 1);
	EXPECT_EQ (out[1].data.catch_class, CC1);
	EXPECT_EQ (out[1].try_start, out[0].try_start);
	EXPECT_EQ (out[1].try_end, out[0].try_end);
	EXPECT_EQ (out[1].handler_start, out[0].handler_start);
}

/*
 * The OUTERMOST clause's own pad names only itself - nothing encloses it - so its
 * run is one entry long and publishes unchanged.
 */
TEST_F (MonoLsdaNesting, OutermostBaseIsNoop)
{
	MonoExceptionClause cl [2];
	std::vector<MonoJitExceptionInfo> out;

	memset (cl, 0, sizeof (cl));
	cl[0].flags = MONO_EXCEPTION_CLAUSE_NONE;
	cl[0].data.catch_class = CC0;
	cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x05;
	cl[1].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
	cl[1].try_offset = 0x10; cl[1].try_len = 0x15; cl[1].handler_offset = 0x25; cl[1].handler_len = 0x05;

	std::vector<MonoLsdaEntry> ents = { { R_START, R_LEN, H_OFF, 1, MONO_EXCEPTION_CLAUSE_FINALLY } };

	ASSERT_TRUE (mono::build_ex_info (ents, cl, 2, base, code_len, out));
	ASSERT_EQ (out.size (), 1u);
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[0].clause_index, 1);
	EXPECT_EQ (out[0].try_start, at (R_START));
	EXPECT_EQ (out[0].handler_start, (gpointer) MINI_ADDR_TO_FTNPTR (base + H_OFF));
}

/*
 * Two clauses that do NOT nest yield two independent one-entry runs over disjoint
 * ranges - the common non-nested method, unchanged.
 */
TEST_F (MonoLsdaNesting, Depth1Unchanged)
{
	MonoExceptionClause cl [2];
	std::vector<MonoJitExceptionInfo> out;

	memset (cl, 0, sizeof (cl));
	cl[0].flags = MONO_EXCEPTION_CLAUSE_NONE;
	cl[0].data.catch_class = CC0;
	cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x05;
	cl[1].flags = MONO_EXCEPTION_CLAUSE_NONE;
	cl[1].data.catch_class = CC1;
	cl[1].try_offset = 0x40; cl[1].try_len = 0x10; cl[1].handler_offset = 0x50; cl[1].handler_len = 0x05;

	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x08, 0x30, 0, MONO_EXCEPTION_CLAUSE_NONE },
		{ 0x60, 0x08, 0x70, 1, MONO_EXCEPTION_CLAUSE_NONE },
	};

	ASSERT_TRUE (mono::build_ex_info (ents, cl, 2, base, code_len, out));
	ASSERT_EQ (out.size (), 2u);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[1].clause_index, 1);
}

/*
 * Multi-call under nesting: the inner catch's try has TWO protected calls, so its
 * pad carries TWO invoke ranges and the gather emits its whole clause list once per
 * range. Four ei as two chains of two - each range's inner catch immediately
 * followed by its enclosing finally over that SAME range.
 *
 * The chains are interleaved rather than blocked (all inner, then all outer)
 * because they are published per range. That is safe precisely because the two
 * ranges are disjoint: no PC matches both, so the runtime's flat walk only ever
 * sees one chain, in order.
 */
TEST_F (MonoLsdaNesting, MultiCallChainPerRange)
{
	MonoExceptionClause cl [2];
	std::vector<MonoJitExceptionInfo> out;

	memset (cl, 0, sizeof (cl));
	cl[0].flags = MONO_EXCEPTION_CLAUSE_NONE;
	cl[0].data.catch_class = CC0;
	cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x05;
	cl[1].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
	cl[1].try_offset = 0x10; cl[1].try_len = 0x15; cl[1].handler_offset = 0x25; cl[1].handler_len = 0x05;

	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x08, 0x40, 0, MONO_EXCEPTION_CLAUSE_NONE },    /* call 1, inner */
		{ 0x10, 0x08, 0x40, 1, MONO_EXCEPTION_CLAUSE_FINALLY }, /* call 1, encloser */
		{ 0x28, 0x08, 0x40, 0, MONO_EXCEPTION_CLAUSE_NONE },    /* call 2, disjoint */
		{ 0x28, 0x08, 0x40, 1, MONO_EXCEPTION_CLAUSE_FINALLY },
	};

	ASSERT_TRUE (mono::build_ex_info (ents, cl, 2, base, code_len, out));
	ASSERT_EQ (out.size (), 4u);

	/* range 1's chain: inner catch then its encloser */
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_NONE);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[1].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[1].clause_index, 1);
	EXPECT_EQ (out[0].try_start, at (0x10));
	EXPECT_EQ (out[1].try_start, out[0].try_start);
	EXPECT_EQ (out[1].try_end, out[0].try_end);
	EXPECT_EQ (out[1].handler_start, out[0].handler_start);

	/* range 2's chain, over its OWN range */
	EXPECT_EQ (out[2].flags, (guint32) MONO_EXCEPTION_CLAUSE_NONE);
	EXPECT_EQ (out[2].clause_index, 0);
	EXPECT_EQ (out[3].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[3].clause_index, 1);
	EXPECT_EQ (out[2].try_start, at (0x28));
	EXPECT_EQ (out[3].try_start, out[2].try_start);
	EXPECT_EQ (out[3].try_end, out[2].try_end);
	EXPECT_EQ (out[3].handler_start, out[2].handler_start);
}

/*
 * SIBLINGS are not nesting. try { } catch(A) catch(B) share the identical protected
 * region and one landing pad, so the pad names both and neither is an encloser of
 * the other. Exactly two ei, in declaration order.
 */
TEST_F (MonoLsdaNesting, SiblingsInDeclarationOrder)
{
	MonoExceptionClause cl [2];
	std::vector<MonoJitExceptionInfo> out;

	memset (cl, 0, sizeof (cl));
	cl[0].flags = MONO_EXCEPTION_CLAUSE_NONE;
	cl[0].data.catch_class = CC0;
	cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x30; cl[0].handler_len = 0x05;
	cl[1].flags = MONO_EXCEPTION_CLAUSE_NONE;
	cl[1].data.catch_class = CC1;
	cl[1].try_offset = 0x10; cl[1].try_len = 0x10; cl[1].handler_offset = 0x40; cl[1].handler_len = 0x05;

	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x10, 0x60, 0, MONO_EXCEPTION_CLAUSE_NONE },
		{ 0x10, 0x10, 0x60, 1, MONO_EXCEPTION_CLAUSE_NONE },
	};

	ASSERT_TRUE (mono::build_ex_info (ents, cl, 2, base, code_len, out));
	ASSERT_EQ (out.size (), 2u);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[1].clause_index, 1);
	EXPECT_EQ (out[0].data.catch_class, CC0);
	EXPECT_EQ (out[1].data.catch_class, CC1);
}

/*
 * Two clauses whose IL try regions CROSS are published as the two independent runs
 * the pads describe. Nothing here reads their try offsets, so a crossing IL shape
 * is not this stage's problem - the translator's gate declines it - and the native
 * ranges are disjoint, so the array is accepted. (The native-range crossing decline
 * is covered by MonoLsdaBuild.OverlappingRangesDecline / StrictNestingDeclines.)
 */
TEST_F (MonoLsdaNesting, CrossingIlNotConsulted)
{
	MonoExceptionClause cl [2];
	std::vector<MonoJitExceptionInfo> out;

	memset (cl, 0, sizeof (cl));
	cl[0].flags = MONO_EXCEPTION_CLAUSE_NONE;
	cl[0].data.catch_class = CC0;
	cl[0].try_offset = 0x10; cl[0].try_len = 0x20; cl[0].handler_offset = 0x50; cl[0].handler_len = 0x05;
	cl[1].flags = MONO_EXCEPTION_CLAUSE_NONE;
	cl[1].data.catch_class = CC1;
	cl[1].try_offset = 0x20; cl[1].try_len = 0x20; cl[1].handler_offset = 0x60; cl[1].handler_len = 0x05;

	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x08, 0x50, 0, MONO_EXCEPTION_CLAUSE_NONE }, /* disjoint native ranges */
		{ 0x80, 0x08, 0x60, 1, MONO_EXCEPTION_CLAUSE_NONE },
	};

	ASSERT_TRUE (mono::build_ex_info (ents, cl, 2, base, code_len, out));
	ASSERT_EQ (out.size (), 2u);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[1].clause_index, 1);
}

/*
 * SIBLING GROUP ENCLOSED BY A FINALLY - the enclosing entry appears EXACTLY ONCE,
 * not once per sibling. try { try {throw} catch(A) catch(B) } finally {}:
 * clause0/clause1 are the inner sibling catches, clause2 the outer finally. All
 * three sit on the ONE inner landing pad, so the run is [A, B, finally] - the
 * finally is named once by the pad, which is what makes the duplicate structurally
 * impossible rather than deduplicated after the fact. Two identical finally ei
 * would make pass-2's first-match-and-continue run it twice when an exception
 * propagates past both siblings (an ECMA-335 12.4.2 violation). Exactly THREE ei -
 * NOT four.
 */
TEST_F (MonoLsdaNesting, SiblingGroupInFinallyOnce)
{
	MonoExceptionClause cl [3];
	std::vector<MonoJitExceptionInfo> out;

	memset (cl, 0, sizeof (cl));
	cl[0].flags = MONO_EXCEPTION_CLAUSE_NONE;    /* inner catch A */
	cl[0].data.catch_class = CC0;
	cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x04;
	cl[1].flags = MONO_EXCEPTION_CLAUSE_NONE;    /* inner catch B (sibling of A) */
	cl[1].data.catch_class = CC1;
	cl[1].try_offset = 0x10; cl[1].try_len = 0x10; cl[1].handler_offset = 0x24; cl[1].handler_len = 0x04;
	cl[2].flags = MONO_EXCEPTION_CLAUSE_FINALLY; /* outer finally over both handlers */
	cl[2].try_offset = 0x10; cl[2].try_len = 0x18; cl[2].handler_offset = 0x28; cl[2].handler_len = 0x04;

	/* One run over one range, from the shared inner landing pad 0x40. */
	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x10, 0x40, 0, MONO_EXCEPTION_CLAUSE_NONE },
		{ 0x10, 0x10, 0x40, 1, MONO_EXCEPTION_CLAUSE_NONE },
		{ 0x10, 0x10, 0x40, 2, MONO_EXCEPTION_CLAUSE_FINALLY },
	};

	ASSERT_TRUE (mono::build_ex_info (ents, cl, 3, base, code_len, out));
	/* A size of 4 would be the double-published finally bug. */
	ASSERT_EQ (out.size (), 3u);

	/* slots 0,1: the two sibling catches, shared range + landing pad */
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_NONE);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[1].flags, (guint32) MONO_EXCEPTION_CLAUSE_NONE);
	EXPECT_EQ (out[1].clause_index, 1);
	EXPECT_EQ (out[0].try_start, at (0x10));
	EXPECT_EQ (out[0].try_end, at (0x20));
	EXPECT_EQ (out[1].try_start, out[0].try_start);
	EXPECT_EQ (out[1].try_end, out[0].try_end);
	EXPECT_EQ (out[0].handler_start, out[1].handler_start);

	/* slot 2: the ONE enclosing finally over the shared range/landing pad */
	EXPECT_EQ (out[2].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[2].clause_index, 2);
	EXPECT_EQ (out[2].try_start, out[0].try_start);
	EXPECT_EQ (out[2].try_end, out[0].try_end);
	EXPECT_EQ (out[2].handler_start, out[0].handler_start);
}

/*
 * The same sibling group with the inner try spanning TWO DISTINCT invoke ranges:
 * the enclosing finally must appear once PER RANGE, not once overall. The pad
 * carries both ranges and the gather emits its clause list for each, so six ei as
 * two chains of three, each over its own range/landing pad.
 */
TEST_F (MonoLsdaNesting, SiblingGroupMultiRangeOnePerRange)
{
	MonoExceptionClause cl [3];
	std::vector<MonoJitExceptionInfo> out;

	memset (cl, 0, sizeof (cl));
	cl[0].flags = MONO_EXCEPTION_CLAUSE_NONE;
	cl[0].data.catch_class = CC0;
	cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x04;
	cl[1].flags = MONO_EXCEPTION_CLAUSE_NONE;
	cl[1].data.catch_class = CC1;
	cl[1].try_offset = 0x10; cl[1].try_len = 0x10; cl[1].handler_offset = 0x24; cl[1].handler_len = 0x04;
	cl[2].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
	cl[2].try_offset = 0x10; cl[2].try_len = 0x18; cl[2].handler_offset = 0x28; cl[2].handler_len = 0x04;

	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x08, 0x40, 0, MONO_EXCEPTION_CLAUSE_NONE },    /* R1 sibling A */
		{ 0x10, 0x08, 0x40, 1, MONO_EXCEPTION_CLAUSE_NONE },    /* R1 sibling B (shared pad) */
		{ 0x10, 0x08, 0x40, 2, MONO_EXCEPTION_CLAUSE_FINALLY }, /* R1 encloser */
		{ 0x28, 0x08, 0x50, 0, MONO_EXCEPTION_CLAUSE_NONE },    /* R2 sibling A */
		{ 0x28, 0x08, 0x50, 1, MONO_EXCEPTION_CLAUSE_NONE },    /* R2 sibling B (shared pad) */
		{ 0x28, 0x08, 0x50, 2, MONO_EXCEPTION_CLAUSE_FINALLY }, /* R2 encloser */
	};

	ASSERT_TRUE (mono::build_ex_info (ents, cl, 3, base, code_len, out));
	ASSERT_EQ (out.size (), 6u); /* two chains of three */

	/* R1's chain */
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[1].clause_index, 1);
	EXPECT_EQ (out[2].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[2].clause_index, 2);
	EXPECT_EQ (out[0].try_start, at (0x10));
	EXPECT_EQ (out[2].try_start, out[0].try_start);
	EXPECT_EQ (out[2].try_end, out[0].try_end);
	EXPECT_EQ (out[2].handler_start, out[0].handler_start);

	/* R2's chain, over its OWN range and pad */
	EXPECT_EQ (out[3].clause_index, 0);
	EXPECT_EQ (out[4].clause_index, 1);
	EXPECT_EQ (out[5].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[5].clause_index, 2);
	EXPECT_EQ (out[3].try_start, at (0x28));
	EXPECT_EQ (out[5].try_start, out[3].try_start);
	EXPECT_EQ (out[5].try_end, out[3].try_end);
	EXPECT_EQ (out[5].handler_start, out[3].handler_start);
	EXPECT_NE (out[2].try_start, out[5].try_start);
}

/*
 * DEPTH-3 try/finally x3 - encloser ORDER at depth >= 3. clause0 (inner) in clause1
 * (middle) in clause2 (outer), all finallys, all named by the inner pad. Order is
 * load-bearing: pass-2 resumes at the running clause's ARRAY slot + 1, so
 * [inner@0, middle@1, outer@2] is what makes the runtime run the finallys
 * inner-to-outer. A DESCENDING chain would give [inner, outer, middle] and run them
 * out of order.
 */
TEST_F (MonoLsdaNesting, Depth3FinallyAscendingOrder)
{
	MonoExceptionClause cl [3];
	std::vector<MonoJitExceptionInfo> out;

	memset (cl, 0, sizeof (cl));
	cl[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY; /* inner */
	cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x04;
	cl[1].flags = MONO_EXCEPTION_CLAUSE_FINALLY; /* middle */
	cl[1].try_offset = 0x10; cl[1].try_len = 0x18; cl[1].handler_offset = 0x28; cl[1].handler_len = 0x04;
	cl[2].flags = MONO_EXCEPTION_CLAUSE_FINALLY; /* outer */
	cl[2].try_offset = 0x10; cl[2].try_len = 0x20; cl[2].handler_offset = 0x30; cl[2].handler_len = 0x04;

	std::vector<MonoLsdaEntry> ents = {
		{ R_START, R_LEN, H_OFF, 0, MONO_EXCEPTION_CLAUSE_FINALLY },
		{ R_START, R_LEN, H_OFF, 1, MONO_EXCEPTION_CLAUSE_FINALLY },
		{ R_START, R_LEN, H_OFF, 2, MONO_EXCEPTION_CLAUSE_FINALLY },
	};

	ASSERT_TRUE (mono::build_ex_info (ents, cl, 3, base, code_len, out));
	ASSERT_EQ (out.size (), 3u);
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[0].try_start, at (R_START));
	EXPECT_EQ (out[0].try_end, at (R_START + R_LEN));
	EXPECT_EQ (out[0].handler_start, (gpointer) MINI_ADDR_TO_FTNPTR (base + H_OFF));
	EXPECT_EQ (out[1].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[1].clause_index, 1);
	EXPECT_EQ (out[2].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[2].clause_index, 2);

	/* every encloser shares the inner clause's range + landing pad */
	EXPECT_EQ (out[1].try_start, out[0].try_start);
	EXPECT_EQ (out[1].try_end, out[0].try_end);
	EXPECT_EQ (out[1].handler_start, out[0].handler_start);
	EXPECT_EQ (out[2].try_start, out[0].try_start);
	EXPECT_EQ (out[2].try_end, out[0].try_end);
	EXPECT_EQ (out[2].handler_start, out[0].handler_start);
}

/* DEPTH-4 - the same ordering over THREE enclosers of one clause. */
TEST_F (MonoLsdaNesting, Depth4FinallyAscendingOrder)
{
	MonoExceptionClause cl [4];
	std::vector<MonoJitExceptionInfo> out;

	memset (cl, 0, sizeof (cl));
	cl[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
	cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x04;
	cl[1].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
	cl[1].try_offset = 0x10; cl[1].try_len = 0x18; cl[1].handler_offset = 0x28; cl[1].handler_len = 0x04;
	cl[2].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
	cl[2].try_offset = 0x10; cl[2].try_len = 0x20; cl[2].handler_offset = 0x30; cl[2].handler_len = 0x04;
	cl[3].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
	cl[3].try_offset = 0x10; cl[3].try_len = 0x28; cl[3].handler_offset = 0x38; cl[3].handler_len = 0x04;

	std::vector<MonoLsdaEntry> ents = {
		{ R_START, R_LEN, H_OFF, 0, MONO_EXCEPTION_CLAUSE_FINALLY },
		{ R_START, R_LEN, H_OFF, 1, MONO_EXCEPTION_CLAUSE_FINALLY },
		{ R_START, R_LEN, H_OFF, 2, MONO_EXCEPTION_CLAUSE_FINALLY },
		{ R_START, R_LEN, H_OFF, 3, MONO_EXCEPTION_CLAUSE_FINALLY },
	};

	ASSERT_TRUE (mono::build_ex_info (ents, cl, 4, base, code_len, out));
	ASSERT_EQ (out.size (), 4u);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[1].clause_index, 1);
	EXPECT_EQ (out[2].clause_index, 2);
	EXPECT_EQ (out[3].clause_index, 3);
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[3].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[1].try_start, out[0].try_start);
	EXPECT_EQ (out[1].handler_start, out[0].handler_start);
	EXPECT_EQ (out[3].try_start, out[0].try_start);
	EXPECT_EQ (out[3].handler_start, out[0].handler_start);
}

/*
 * A sibling catch group enclosed by TWO finallys - siblings and depth >= 3
 * composing. One pad names all four clauses, so the run is [A, B, middle, outer]
 * and each encloser appears exactly once, after both siblings, in nesting order.
 */
TEST_F (MonoLsdaNesting, Depth3SiblingAscending)
{
	MonoExceptionClause cl [4];
	std::vector<MonoJitExceptionInfo> out;

	memset (cl, 0, sizeof (cl));
	cl[0].flags = MONO_EXCEPTION_CLAUSE_NONE;    /* inner catch A */
	cl[0].data.catch_class = CC0;
	cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x04;
	cl[1].flags = MONO_EXCEPTION_CLAUSE_NONE;    /* inner catch B (sibling of A) */
	cl[1].data.catch_class = CC1;
	cl[1].try_offset = 0x10; cl[1].try_len = 0x10; cl[1].handler_offset = 0x24; cl[1].handler_len = 0x04;
	cl[2].flags = MONO_EXCEPTION_CLAUSE_FINALLY; /* middle finally */
	cl[2].try_offset = 0x10; cl[2].try_len = 0x18; cl[2].handler_offset = 0x28; cl[2].handler_len = 0x04;
	cl[3].flags = MONO_EXCEPTION_CLAUSE_FINALLY; /* outer finally */
	cl[3].try_offset = 0x10; cl[3].try_len = 0x20; cl[3].handler_offset = 0x30; cl[3].handler_len = 0x04;

	std::vector<MonoLsdaEntry> ents = {
		{ 0x10, 0x10, 0x40, 0, MONO_EXCEPTION_CLAUSE_NONE },
		{ 0x10, 0x10, 0x40, 1, MONO_EXCEPTION_CLAUSE_NONE },
		{ 0x10, 0x10, 0x40, 2, MONO_EXCEPTION_CLAUSE_FINALLY },
		{ 0x10, 0x10, 0x40, 3, MONO_EXCEPTION_CLAUSE_FINALLY },
	};

	ASSERT_TRUE (mono::build_ex_info (ents, cl, 4, base, code_len, out));
	ASSERT_EQ (out.size (), 4u); /* 2 sibling catches + 2 enclosers once each */

	/* slots 0,1: the two sibling catches over the shared range/pad */
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_NONE);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[1].flags, (guint32) MONO_EXCEPTION_CLAUSE_NONE);
	EXPECT_EQ (out[1].clause_index, 1);
	EXPECT_EQ (out[0].try_start, out[1].try_start);
	EXPECT_EQ (out[0].handler_start, out[1].handler_start);

	/* slots 2,3: the two enclosing finallys, once each, innermost-first */
	EXPECT_EQ (out[2].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[2].clause_index, 2);
	EXPECT_EQ (out[3].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[3].clause_index, 3);
	EXPECT_EQ (out[2].try_start, out[0].try_start);
	EXPECT_EQ (out[2].handler_start, out[0].handler_start);
	EXPECT_EQ (out[3].try_start, out[0].try_start);
	EXPECT_EQ (out[3].handler_start, out[0].handler_start);
}

/* ------------------------------------------------------------ guard cases */

/*
 * The thread-abort guard entries build_ex_info appends for a FINALLY clause, one
 * per PC range the recovery found the handler body occupying.
 *
 * What these pin down is that a guard entry is inert for dispatch (empty try range)
 * while still carrying the handler extent and exvar the runtime's guard reads, and
 * that a clause gets one entry per body copy rather than a single span over all of
 * them.
 */
class MonoLsdaGuards : public LsdaJoinTest {
protected:
	void
	SetUp () override
	{
		memset (clauses, 0, sizeof (clauses));
		clauses[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
		clauses[1].flags = MONO_EXCEPTION_CLAUSE_NONE;
		clauses[1].data.catch_class = CC0;
	}

	MonoExceptionClause clauses [2];
};

/*
 * A finally whose try region has no protected call publishes no dispatch entry at
 * all - the shape that left the guard uninstallable. The guard entry must appear
 * anyway, which is the whole point of it being separate.
 */
TEST_F (MonoLsdaGuards, NoDispatchEntry)
{
	std::vector<MonoFinallyGuard> guards = { { 0, 0x20, 0x30, -24 } };
	std::vector<MonoJitExceptionInfo> out;

	ASSERT_TRUE (mono::build_ex_info ({}, clauses, 2, base, code_len, out, guards));
	ASSERT_EQ (out.size (), 1u);
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_FINALLY);
	EXPECT_EQ (out[0].clause_index, 0);
	/* Empty try range: is_address_protected () is false for every PC. */
	EXPECT_EQ (out[0].try_start, at (0));
	EXPECT_EQ (out[0].try_end, at (0));
	EXPECT_EQ (out[0].handler_start, at (0x20));
	EXPECT_EQ (out[0].data.handler_end, at (0x30));
	EXPECT_EQ (out[0].exvar_offset, -24);
}

/*
 * A duplicated body: two ranges for one clause, published as two entries with the
 * same exvar. A single span [0x20,0x90) would have covered the unrelated code
 * between them and made the guard match PCs that are not in the finally at all.
 */
TEST_F (MonoLsdaGuards, DuplicatedBody)
{
	std::vector<MonoLsdaEntry> ents = { { 0x10, 0x08, 0x40, 1 } };
	std::vector<MonoFinallyGuard> guards = {
		{ 0, 0x20, 0x30, -24 },
		{ 0, 0x80, 0x90, -24 },
	};
	std::vector<MonoJitExceptionInfo> out;

	ASSERT_TRUE (mono::build_ex_info (ents, clauses, 2, base, code_len, out, guards));
	ASSERT_EQ (out.size (), 3u);
	/* The dispatch entry keeps slot 0; guards are appended after it. */
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_NONE);
	EXPECT_EQ (out[1].handler_start, at (0x20));
	EXPECT_EQ (out[1].data.handler_end, at (0x30));
	EXPECT_EQ (out[2].handler_start, at (0x80));
	EXPECT_EQ (out[2].data.handler_end, at (0x90));
	EXPECT_EQ (out[1].exvar_offset, -24);
	EXPECT_EQ (out[2].exvar_offset, -24);
}

/*
 * Two clauses each with their own body, to pin that a guard entry is keyed to the
 * right clause when there is more than one to confuse it with.
 */
TEST_F (MonoLsdaGuards, TwoClauses)
{
	MonoExceptionClause two_finally [2];
	std::vector<MonoJitExceptionInfo> out;

	memset (two_finally, 0, sizeof (two_finally));
	two_finally[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
	two_finally[1].flags = MONO_EXCEPTION_CLAUSE_FINALLY;

	std::vector<MonoFinallyGuard> guards = {
		{ 0, 0x20, 0x30, -24 },
		{ 1, 0x60, 0x70, -32 },
	};

	ASSERT_TRUE (mono::build_ex_info ({}, two_finally, 2, base, code_len, out, guards));
	ASSERT_EQ (out.size (), 2u);
	EXPECT_EQ (out[0].clause_index, 0);
	EXPECT_EQ (out[0].exvar_offset, -24);
	EXPECT_EQ (out[0].handler_start, at (0x20));
	EXPECT_EQ (out[1].clause_index, 1);
	EXPECT_EQ (out[1].exvar_offset, -32);
	EXPECT_EQ (out[1].handler_start, at (0x60));
}

/*
 * The base register is per guard, not per method - two copies of one body can be
 * homed against different registers.
 */
TEST_F (MonoLsdaGuards, BaseReg)
{
	/* 4/5 are AMD64_RSP/AMD64_RBP; this only checks that the byte survives. */
	std::vector<MonoFinallyGuard> guards = {
		{ 0, 0x20, 0x30, -24, 4 },
		{ 0, 0x80, 0x90, -24, 5 },
	};
	std::vector<MonoJitExceptionInfo> out;

	ASSERT_TRUE (mono::build_ex_info ({}, clauses, 2, base, code_len, out, guards));
	ASSERT_EQ (out.size (), 2u);
	EXPECT_EQ (out[0].exvar_offset, -24);
	EXPECT_EQ (out[0].exvar_base_reg, AMD64_RSP);
	EXPECT_EQ (out[1].exvar_offset, -24);
	EXPECT_EQ (out[1].exvar_base_reg, AMD64_RBP);
}

/* A catch-only method passes no guards and is unaffected. */
TEST_F (MonoLsdaGuards, AbsentForCatch)
{
	std::vector<MonoLsdaEntry> ents = { { 0x10, 0x08, 0x40, 1 } };
	std::vector<MonoJitExceptionInfo> out;

	ASSERT_TRUE (mono::build_ex_info (ents, clauses, 2, base, code_len, out));
	ASSERT_EQ (out.size (), 1u);
	/* data is a union - a catch entry's slot holds catch_class, not an extent. */
	EXPECT_EQ (out[0].flags, (guint32) MONO_EXCEPTION_CLAUSE_NONE);
	EXPECT_EQ (out[0].data.catch_class, CC0);
	EXPECT_EQ (out[0].exvar_offset, 0);
}

} // namespace test
} // namespace mono
