/*
 * test-llvm-ehtable.cpp: unit tests for the `.mono_lsda` publish/validate core
 * in mono/mini/llvm/mono_lsda.cpp.
 *
 * parse_mono_lsda() decodes the target-neutral `.mono_lsda` section
 * MonoLSDAStreamer (engine.cpp, C3) emits; build_ex_info() validates those
 * tuples against the IL clause table and joins them into a
 * MonoJitExceptionInfo[] (the pure core of publish_mono_lsda, factored out so
 * it needs no MonoCompile). Everything here drives them with byte buffers and
 * synthetic clause tables, an OFFLINE style similar to how test-llvm-ehframe.c
 * drives the .eh_frame transcoder.
 *
 * ---- the guard-page runner ----
 *
 * Every buffer is decoded with its last byte flush against a PROT_NONE guard
 * page (parse_guarded), so any read one byte past the declared size faults
 * immediately with a legible crash rather than reading adjacent memory and
 * returning a plausible-but-wrong answer. The truncation sweep - decoding every
 * prefix length of a valid section - leans on this: a decoder that forgets a
 * bounds check is caught here, not left to a later heisenbug.
 */

#include "config.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <vector>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#ifdef ENABLE_LLVM

#include "mini/llvm/mono_lsda.hpp"

using mono::MonoLsdaEntry;
using mono::MonoFinallyGuard;

/* ------------------------------------------------------------ reporting */

static int failures;
static int cases_run;
static const char * volatile current_case = "(startup)";

/* ------------------------------------------------- crash legibility */

#if defined (HAVE_UNISTD_H) && !defined (HOST_WIN32)

static void
sig_write (const char *s)
{
	ssize_t ignored = write (2, s, strlen (s));
	(void) ignored;
}

static void
crash_handler (int sig)
{
	sig_write ("\n*** test-llvm-ehtable died on signal ");
	sig_write (sig == SIGSEGV ? "SIGSEGV" : "SIGBUS");
	sig_write (" in case: ");
	sig_write ((const char*) current_case);
	sig_write ("\n*** A fault here means the decoder read PAST the end of the LSDA"
	           "\n*** buffer it was given (its last byte sits against a guard page).\n");
	signal (sig, SIG_DFL);
	raise (sig);
}

static void
install_crash_handler (void)
{
	signal (SIGSEGV, crash_handler);
	signal (SIGBUS, crash_handler);
}

#else
static void install_crash_handler (void) { }
#endif

/* ============================================================================
 * mono_lsda.cpp - the load-time .mono_lsda publish/validate core (plan 12 C4).
 *
 * parse_mono_lsda() decodes the target-neutral `.mono_lsda` section
 * MonoLSDAStreamer (engine.cpp, C3) emits; build_ex_info() validates those
 * tuples against the IL clause table and joins them into a MonoJitExceptionInfo[]
 * (the pure core of publish_mono_lsda, factored out so it needs no MonoCompile).
 * Everything here drives them with byte buffers and synthetic clause tables,
 * an OFFLINE style. Expectations are hand-derived from the format, not echoed
 * from the implementation.
 * ==========================================================================*/

/* Little-endian append helpers for assembling .mono_lsda byte vectors. */
static void
put_u16 (std::vector<std::uint8_t> &b, std::uint16_t v)
{
	b.push_back ((std::uint8_t) (v & 0xff));
	b.push_back ((std::uint8_t) ((v >> 8) & 0xff));
}

static void
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
static std::vector<std::uint8_t>
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
 * Parse LEN bytes of DATA with the final byte flush against a PROT_NONE guard
 * page: any over-read faults loudly instead of returning a plausible-but-wrong
 * decode.
 */
static bool
parse_guarded (const std::uint8_t *data, std::size_t len, std::vector<MonoLsdaEntry> &out)
{
#if defined (HAVE_SYS_MMAN_H) && !defined (HOST_WIN32)
	long pagel = sysconf (_SC_PAGESIZE);
	std::size_t page = pagel > 0 ? (std::size_t) pagel : 4096;

	if (len <= page) {
		std::uint8_t *base = (std::uint8_t*) mmap (nullptr, 2 * page,
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base != MAP_FAILED) {
			if (mprotect (base + page, page, PROT_NONE) == 0) {
				std::uint8_t *buf = base + page - len; /* last byte at page-1 */
				if (len)
					memcpy (buf, data, len);
				bool r = mono::parse_mono_lsda (buf, len, out);
				munmap (base, 2 * page);
				return r;
			}
			munmap (base, 2 * page);
		}
	}
#endif
	std::vector<std::uint8_t> buf (data, data + len);
	return mono::parse_mono_lsda (buf.empty () ? nullptr : buf.data (), len, out);
}

/* ------------------------------------------------------------ parse cases */

/*
 * The exact bytes MonoLSDAStreamer emits for the v2 format (self-describing kind
 * column). Same catch geometry as plan 12 1.2's probe2.o dump, now version 2 with
 * a trailing per-entry kind == 0 (catch):
 *   44534c4d 02000200   magic 'MLSD', version 2, count 2
 *   01000000 05000000 11000000 07000000 00000000  {try=1, len=5, h=0x11, clause=7, kind=0}
 *   06000000 05000000 0f000000 03000000 00000000  {try=6, len=5, h=0x0f, clause=3, kind=0}
 * 48 bytes = 8 + 2*20. Decoded by hand from the format above.
 */
static const std::uint8_t GOLDEN_MLSD [] = {
	0x44, 0x53, 0x4c, 0x4d, 0x02, 0x00, 0x02, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	0x11, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
	0x06, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	0x0f, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,
};

static void
check_entry (const char *what, int i, const MonoLsdaEntry &got, const MonoLsdaEntry &exp)
{
	if (got.try_start_off != exp.try_start_off || got.try_len != exp.try_len ||
	    got.handler_off != exp.handler_off || got.clause_index != exp.clause_index ||
	    got.kind != exp.kind) {
		printf ("FAIL %s: entry %d: got {ts=%u tl=%u h=%u ci=%u k=%u}, "
		        "want {ts=%u tl=%u h=%u ci=%u k=%u}\n", what, i,
		        got.try_start_off, got.try_len, got.handler_off, got.clause_index,
		        got.kind,
		        exp.try_start_off, exp.try_len, exp.handler_off, exp.clause_index,
		        exp.kind);
		failures ++;
	}
}

static void
expect_parse (const char *what, const std::uint8_t *data, std::size_t len,
              const MonoLsdaEntry *exp, int nexp)
{
	std::vector<MonoLsdaEntry> out;
	current_case = what;
	cases_run ++;
	if (!parse_guarded (data, len, out)) {
		printf ("FAIL %s: declined a valid section\n", what);
		failures ++;
		return;
	}
	if ((int) out.size () != nexp) {
		printf ("FAIL %s: entry count: got %d, want %d\n", what, (int) out.size (), nexp);
		failures ++;
		return;
	}
	int before = failures;
	for (int i = 0; i < nexp; ++i)
		check_entry (what, i, out[i], exp[i]);
	if (failures == before)
		printf ("ok   %s (%d entries)\n", what, nexp);
}

static void
expect_parse_decline (const char *what, const std::uint8_t *data, std::size_t len)
{
	std::vector<MonoLsdaEntry> out;
	current_case = what;
	cases_run ++;
	if (parse_guarded (data, len, out)) {
		printf ("FAIL %s: accepted a section that should decline\n", what);
		failures ++;
	} else {
		printf ("ok   %s (declined)\n", what);
	}
}

/*
 * parse_mono_lsda's final "unreachable given exact-size, but bounds-honest"
 * !r.ok() check (mono_lsda.cpp 164-165) is not exercised as a standalone case:
 * the exact-size check just above it already guarantees size == 8 + count*20,
 * so every u32() read in the entry loop always has its 4 bytes available and
 * that branch cannot be reached through the section's byte layout alone (it
 * would need an injectable Reader to force ok()==false mid-loop despite a
 * passing exact-size check). That is overkill for this format; the check
 * stays as defensive bounds-honesty, acknowledged here rather than tested.
 */

static void
cases_mono_lsda_parse (void)
{
	/* The C3 golden vector decodes to its two hand-derived entries (kind 0). */
	static const MonoLsdaEntry golden_exp [] = {
		{ 1, 5, 0x11, 7, 0 },
		{ 6, 5, 0x0f, 3, 0 },
	};
	expect_parse ("mlsd-golden-two-entry", GOLDEN_MLSD, sizeof (GOLDEN_MLSD),
	              golden_exp, 2);

	/* A header-only section (count 0) is well-formed and decodes to nothing. */
	{
		std::vector<std::uint8_t> b = make_lsda (0x4d4c5344u, 2, 0, {});
		expect_parse ("mlsd-count-zero-header-only", b.data (), b.size (), nullptr, 0);
	}

	/* One-entry section: exactly 8 + 20 bytes. Non-zero kind round-trips verbatim. */
	{
		std::vector<MonoLsdaEntry> ents = { { 0x20, 0x08, 0x30, 0, 2 } };
		std::vector<std::uint8_t> b = make_lsda (0x4d4c5344u, 2, 1, ents);
		static const MonoLsdaEntry one_exp [] = { { 0x20, 0x08, 0x30, 0, 2 } };
		expect_parse ("mlsd-one-entry", b.data (), b.size (), one_exp, 1);
	}

	/* --- negatives --- */

	/* Bad magic. */
	{
		std::vector<MonoLsdaEntry> ents = { { 1, 5, 0x11, 7, 0 } };
		std::vector<std::uint8_t> b = make_lsda (0xdeadbeefu, 2, 1, ents);
		expect_parse_decline ("mlsd-bad-magic", b.data (), b.size ());
	}

	/*
	 * A v1 buffer now DECLINES against this v2-only loader (CAP-EH-0). It is a
	 * genuine 16-byte-stride v1 record (magic ok, version 1, one 16-byte entry):
	 * the loader recognises only version 2, so the older format is refused rather
	 * than misread at the wrong stride.
	 */
	{
		std::vector<std::uint8_t> b;
		put_u32 (b, 0x4d4c5344u); /* magic 'MLSD' */
		put_u16 (b, 1);           /* version 1 */
		put_u16 (b, 1);           /* count 1 */
		put_u32 (b, 1); put_u32 (b, 5); put_u32 (b, 0x11); put_u32 (b, 7); /* one v1 16B entry */
		expect_parse_decline ("mlsd-v1-declines", b.data (), b.size ());
	}

	/* Any other unrecognised version (3) declines too. */
	{
		std::vector<MonoLsdaEntry> ents = { { 1, 5, 0x11, 7, 0 } };
		std::vector<std::uint8_t> b = make_lsda (0x4d4c5344u, 3, 1, ents);
		expect_parse_decline ("mlsd-version-3-declines", b.data (), b.size ());
	}

	/* Truncated header (7 bytes: count field cut short). */
	{
		std::vector<std::uint8_t> b = make_lsda (0x4d4c5344u, 2, 0, {});
		b.pop_back ();
		expect_parse_decline ("mlsd-truncated-header", b.data (), b.size ());
	}

	/* Truncated entry: header says 2 entries but only one entry's worth of
	 * payload is present (8 + 20 bytes). Exact-size mismatch (28 != 8+2*20)
	 * declines. */
	{
		std::vector<MonoLsdaEntry> ents = { { 1, 5, 0x11, 7, 0 } };
		std::vector<std::uint8_t> b = make_lsda (0x4d4c5344u, 2, 2, ents);
		/* count field is 2 but only one 20-byte entry follows -> size 28 != 48 */
		expect_parse_decline ("mlsd-truncated-entry", b.data (), b.size ());
	}

	/*
	 * THE EXACT-SIZE / TWO-RECORD DECLINE (plan 12 3, C4's belt-and-suspenders).
	 * Two full method records concatenated: the first header declares count 1
	 * (expected size 24) but the buffer is 48 bytes. A longer-than-exact section
	 * means the one-method-per-module invariant broke; reading only the first
	 * record would misattribute clause geometry, so parse declines.
	 */
	{
		std::vector<MonoLsdaEntry> ents = { { 1, 5, 0x11, 7, 0 } };
		std::vector<std::uint8_t> rec = make_lsda (0x4d4c5344u, 2, 1, ents);
		std::vector<std::uint8_t> two = rec;
		two.insert (two.end (), rec.begin (), rec.end ()); /* 28 + 28 = 56 bytes */
		expect_parse_decline ("mlsd-two-record-oversize-declines", two.data (), two.size ());
	}

	/*
	 * Trailing-byte oversize: exactly one valid record plus a single junk byte.
	 * 29 != 28 -> decline (a section MUST be exactly its declared extent).
	 */
	{
		std::vector<MonoLsdaEntry> ents = { { 1, 5, 0x11, 7, 0 } };
		std::vector<std::uint8_t> b = make_lsda (0x4d4c5344u, 2, 1, ents);
		b.push_back (0xaa);
		expect_parse_decline ("mlsd-one-trailing-byte-oversize-declines", b.data (), b.size ());
	}

	/* Null pointer and zero length both decline without touching memory. */
	{
		std::vector<MonoLsdaEntry> out;
		current_case = "mlsd-null-and-empty";
		cases_run ++;
		if (mono::parse_mono_lsda (nullptr, 0, out)) {
			printf ("FAIL mlsd-null-and-empty: accepted null\n");
			failures ++;
		} else {
			const std::uint8_t nothing [1] = { 0 };
			if (parse_guarded (nothing, 0, out)) {
				printf ("FAIL mlsd-null-and-empty: accepted zero-length\n");
				failures ++;
			} else {
				printf ("ok   mlsd-null-and-empty (declined)\n");
			}
		}
	}

	/*
	 * Truncation sweep: every proper prefix of the C3 golden vector must decline
	 * and must not read past its guarded end (a forgotten bounds check faults
	 * here, not in a later heisenbug).
	 */
	{
		bool ok = true;
		for (std::size_t len = 0; len < sizeof (GOLDEN_MLSD); ++len) {
			std::vector<MonoLsdaEntry> out;
			current_case = "mlsd-truncation-sweep";
			if (parse_guarded (GOLDEN_MLSD, len, out)) {
				printf ("FAIL mlsd-truncation-sweep: prefix len %zu accepted\n", len);
				failures ++;
				ok = false;
				break;
			}
		}
		cases_run ++;
		if (ok)
			printf ("ok   mlsd-truncation-sweep (%zu prefixes declined)\n",
			        sizeof (GOLDEN_MLSD) - 1);
	}
}

/* ------------------------------------------------------------ build cases */

/* Sentinel catch_class pointers (never dereferenced; build_ex_info only copies). */
static MonoClass * const CC0 = (MonoClass *) (std::uintptr_t) 0xC0FFEE00u;
static MonoClass * const CC1 = (MonoClass *) (std::uintptr_t) 0xC0FFEE11u;

/*
 * Assert build_ex_info accepts ENTRIES against a synthetic clause table and
 * produces the hand-derived MonoJitExceptionInfo[]. NATIVE_CODE is a real
 * CODE_LEN-byte buffer so try_start/try_end/handler_start are valid pointers to
 * check against BASE + offset.
 */
static void
cases_mono_lsda_build (void)
{
	const std::uint32_t code_len = 0x100;
	std::vector<std::uint8_t> code (code_len, 0);
	const std::uint8_t *base = code.data ();

	/* Two catch clauses (both CLAUSE_NONE) with distinct catch_class sentinels. */
	MonoExceptionClause clauses [2];
	memset (clauses, 0, sizeof (clauses));
	clauses[0].flags = MONO_EXCEPTION_CLAUSE_NONE;
	clauses[0].data.catch_class = CC0;
	clauses[1].flags = MONO_EXCEPTION_CLAUSE_NONE;
	clauses[1].data.catch_class = CC1;

	/* --- valid: two disjoint entries joining onto the two clauses --- */
	{
		std::vector<MonoLsdaEntry> ents = {
			{ 0x10, 0x20, 0x40, 0 }, /* [0x10,0x30) -> handler 0x40, clause 0 */
			{ 0x50, 0x10, 0x80, 1 }, /* [0x50,0x60) -> handler 0x80, clause 1 */
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-valid-two-clause";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, clauses, 2, base, code_len, out);
		if (!ok || out.size () != 2) {
			printf ("FAIL build-valid-two-clause: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			const MonoJitExceptionInfo &e0 = out[0];
			const MonoJitExceptionInfo &e1 = out[1];
			bool good =
				e0.flags == MONO_EXCEPTION_CLAUSE_NONE &&
				e0.clause_index == 0 &&
				e0.try_start == (gpointer) (base + 0x10) &&
				e0.try_end == (gpointer) (base + 0x30) &&
				e0.handler_start == (gpointer) (base + 0x40) &&
				e0.data.catch_class == CC0 &&
				e0.exvar_offset == 0 &&
				e0.try_offset == 0 && e0.try_len == 0 &&
				e0.handler_offset == 0 && e0.handler_len == 0 &&
				e1.flags == MONO_EXCEPTION_CLAUSE_NONE &&
				e1.clause_index == 1 &&
				e1.try_start == (gpointer) (base + 0x50) &&
				e1.try_end == (gpointer) (base + 0x60) &&
				e1.handler_start == (gpointer) (base + 0x80) &&
				e1.data.catch_class == CC1;
			if (!good) {
				printf ("FAIL build-valid-two-clause: field mismatch\n");
				failures ++;
			} else {
				printf ("ok   build-valid-two-clause (2 ei)\n");
			}
		}
	}

	/*
	 * Multi-call shape: two disjoint entries sharing ONE clause/handler (a try
	 * with two protected calls). Both must publish, same clause_index/handler,
	 * different try_start - mono's is_address_protected takes the first PC match.
	 */
	{
		std::vector<MonoLsdaEntry> ents = {
			{ 0x10, 0x08, 0x40, 0 },
			{ 0x30, 0x08, 0x40, 0 },
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-multi-call-shared-clause";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, clauses, 2, base, code_len, out);
		bool good = ok && out.size () == 2 &&
			out[0].clause_index == 0 && out[1].clause_index == 0 &&
			out[0].handler_start == out[1].handler_start &&
			out[0].try_start != out[1].try_start &&
			out[0].try_start == (gpointer) (base + 0x10) &&
			out[1].try_start == (gpointer) (base + 0x30);
		if (!good) {
			printf ("FAIL build-multi-call-shared-clause: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   build-multi-call-shared-clause (2 ei, clause 0)\n");
		}
	}

	/* Helper for the decline cases. */
	auto expect_build_decline = [&] (const char *what,
	                                 const std::vector<MonoLsdaEntry> &ents,
	                                 const MonoExceptionClause *cls, int nclauses) {
		std::vector<MonoJitExceptionInfo> out;
		current_case = what;
		cases_run ++;
		if (mono::build_ex_info (ents, cls, nclauses, base, code_len, out)) {
			printf ("FAIL %s: accepted entries that should decline\n", what);
			failures ++;
		} else {
			printf ("ok   %s (declined)\n", what);
		}
	};

	/* try_start_off == code_len (past the code). */
	expect_build_decline ("build-try-start-past-code",
		{ { code_len, 0x00, 0x40, 0 } }, clauses, 2);

	/* try_start_off + try_len past code_len (the 64-bit sum check). */
	expect_build_decline ("build-try-end-past-code",
		{ { code_len - 0x10, 0x20, 0x40, 0 } }, clauses, 2);

	/*
	 * The 64-bit-sum guard, adversarially: try_start_off + try_len is chosen so
	 * a 32-bit-wrapped sum would wrongly stay in range (0xFFFFFFF0 + 0x20 wraps
	 * mod 2^32 to 0x10, comfortably under code_len), but the true 64-bit sum
	 * (0x100000010) correctly exceeds code_len (set to 0xFFFFFFFF for this one
	 * call only). This must still decline.
	 */
	{
		std::vector<MonoLsdaEntry> ents = { { 0xFFFFFFF0u, 0x20u, 0x40u, 0 } };
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-try-range-32bit-overflow";
		cases_run ++;
		if (mono::build_ex_info (ents, clauses, 2, base, 0xFFFFFFFFu, out)) {
			printf ("FAIL build-try-range-32bit-overflow: accepted entries that should decline\n");
			failures ++;
		} else {
			printf ("ok   build-try-range-32bit-overflow (declined)\n");
		}
	}

	/* handler_off == code_len (past the code). */
	expect_build_decline ("build-handler-past-code",
		{ { 0x10, 0x08, code_len, 0 } }, clauses, 2);

	/*
	 * The vacuous no-EH-method path: no clause table and no entries is a
	 * trivial accept (the num_clauses>0-with-no-entries fail-safe does not
	 * apply when there IS no clause table), producing an empty published array.
	 */
	{
		std::vector<MonoLsdaEntry> ents;
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-no-clauses-no-entries-accept";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, nullptr, 0, base, code_len, out);
		if (!ok || !out.empty ()) {
			printf ("FAIL build-no-clauses-no-entries-accept: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   build-no-clauses-no-entries-accept (0 ei)\n");
		}
	}

	/*
	 * FAULT is ADMITTED by F2 (fully correct - the runtime reads neither
	 * handler_end nor exvar_offset for a fault clause). Entry kind FAULT agrees
	 * with the join; assert the published ei carries flags FAULT, the joined
	 * geometry, and data.handler_end / exvar_offset both 0.
	 */
	{
		MonoExceptionClause flt [1];
		memset (flt, 0, sizeof (flt));
		flt[0].flags = MONO_EXCEPTION_CLAUSE_FAULT;
		std::vector<MonoLsdaEntry> ents = { { 0x10, 0x08, 0x40, 0, MONO_EXCEPTION_CLAUSE_FAULT } };
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-fault-admits";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, flt, 1, base, code_len, out);
		bool good = ok && out.size () == 1 &&
			out[0].flags == MONO_EXCEPTION_CLAUSE_FAULT &&
			out[0].clause_index == 0 &&
			out[0].try_start == (gpointer) (base + 0x10) &&
			out[0].try_end == (gpointer) (base + 0x18) &&
			out[0].handler_start == (gpointer) (base + 0x40) &&
			out[0].data.handler_end == 0 &&
			out[0].exvar_offset == 0;
		if (!good) {
			printf ("FAIL build-fault-admits: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   build-fault-admits (1 ei, handler_end/exvar 0)\n");
		}
	}

	/*
	 * FINALLY is ADMITTED by F2 once its entry kind (2) AGREES with the join. This
	 * is the "safe quiet-gap intermediate": the abort-guard fields
	 * data.handler_end and exvar_offset are published as 0 together (the §1.3
	 * invariant - both real or both 0; F2 uses both 0, F4 supplies both via the
	 * stackmap sideband). Assert flags FINALLY, joined geometry, and both guard
	 * fields 0.
	 */
	{
		MonoExceptionClause fin [1];
		memset (fin, 0, sizeof (fin));
		fin[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
		std::vector<MonoLsdaEntry> ents = { { 0x10, 0x08, 0x40, 0, MONO_EXCEPTION_CLAUSE_FINALLY } };
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-finally-admits-guard-fields-zero";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, fin, 1, base, code_len, out);
		bool good = ok && out.size () == 1 &&
			out[0].flags == MONO_EXCEPTION_CLAUSE_FINALLY &&
			out[0].clause_index == 0 &&
			out[0].try_start == (gpointer) (base + 0x10) &&
			out[0].try_end == (gpointer) (base + 0x18) &&
			out[0].handler_start == (gpointer) (base + 0x40) &&
			out[0].data.handler_end == 0 &&
			out[0].exvar_offset == 0;
		if (!good) {
			printf ("FAIL build-finally-admits-guard-fields-zero: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   build-finally-admits-guard-fields-zero (1 ei, handler_end/exvar 0)\n");
		}
	}

	/*
	 * count == 0 while num_clauses > 0: every protected call in this method's
	 * IL clause(s) was optimized to a nounwind call, so nothing survived that
	 * could ever reach a handler. Confirmed safe, not uncertain - accept with
	 * an empty published array rather than decline.
	 */
	{
		std::vector<MonoLsdaEntry> ents;
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-empty-while-clauses-accept";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, clauses, 2, base, code_len, out);
		if (!ok || !out.empty ()) {
			printf ("FAIL build-empty-while-clauses-accept: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   build-empty-while-clauses-accept (0 ei)\n");
		}
	}

	/*
	 * An entries set containing ONLY a resume-pad marker for a FINALLY clause
	 * is the same confirmed-safe case as an empty entries set, just reached
	 * through a different path: the clause's OWN try-body had every protected
	 * call optimized away (nothing left for it to contribute as a base entry),
	 * but it has an encloser, so its resume-pad invoke still gets emitted
	 * unconditionally (emit_resume_unwind, translator-call.cpp). Accept with
	 * an empty published array, not decline.
	 */
	{
		MonoExceptionClause fin [1];
		memset (fin, 0, sizeof (fin));
		fin[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
		std::vector<MonoLsdaEntry> ents = { { 0x10, 0x08, 0x40, 0, mono::MONO_LSDA_KIND_RESUME_PAD } };
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-resume-pad-only-accept";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, fin, 1, base, code_len, out);
		if (!ok || !out.empty ()) {
			printf ("FAIL build-resume-pad-only-accept: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   build-resume-pad-only-accept (0 ei)\n");
		}
	}

	/*
	 * SIBLING CATCHES: try { } catch(A) catch(B) is one landing pad with two
	 * TypeIds over ONE invoke range, so C2/C3 emit two entries with the SAME
	 * range and DIFFERENT clause_index. Both must publish (equal-or-disjoint
	 * invariant) - mono matches the shared PC range for both, then picks the type
	 * by catch_class with RDX = clause_index. Assert ACCEPT, both ei sharing the
	 * range, distinct clause_index/catch_class, correct handler_start.
	 */
	{
		std::vector<MonoLsdaEntry> ents = {
			{ 0x10, 0x20, 0x60, 0 }, /* catch A: [0x10,0x30) -> handler 0x60, clause 0 */
			{ 0x10, 0x20, 0x90, 1 }, /* catch B: SAME range     -> handler 0x90, clause 1 */
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-sibling-same-range";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, clauses, 2, base, code_len, out);
		bool good = ok && out.size () == 2 &&
			out[0].try_start == (gpointer) (base + 0x10) &&
			out[0].try_end == (gpointer) (base + 0x30) &&
			out[1].try_start == (gpointer) (base + 0x10) &&
			out[1].try_end == (gpointer) (base + 0x30) &&
			out[0].clause_index == 0 && out[1].clause_index == 1 &&
			out[0].data.catch_class == CC0 && out[1].data.catch_class == CC1 &&
			out[0].handler_start == (gpointer) (base + 0x60) &&
			out[1].handler_start == (gpointer) (base + 0x90);
		if (!good) {
			printf ("FAIL build-sibling-same-range: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   build-sibling-same-range (2 ei, shared range, clauses 0/1)\n");
		}
	}

	/*
	 * SECTION ORDER within one pad's run is authoritative - build_ex_info must NOT
	 * re-sort it by clause_index. The order comes from the landing pad's own operand
	 * list (add_covering_clauses, translator-call.cpp) with the gather undoing LLVM's
	 * reversal, so it is already declaration-order for siblings and innermost-first
	 * for enclosers.
	 *
	 * Re-sorting on clause_index would look harmless while every clause belongs to
	 * the one method being compiled, since ECMA-335 12.4.2.5 makes ascending index
	 * mean innermost-first there. It stops being true the moment a chain spans an
	 * inlined body, whose clauses are numbered independently of the caller's - so a
	 * clause_index sort key would silently reorder a cross-method nest. Feed a run
	 * whose indices DESCEND and assert it publishes exactly as given.
	 */
	{
		std::vector<MonoLsdaEntry> ents = {
			{ 0x10, 0x20, 0x60, 1 }, /* clause 1 first in the run... */
			{ 0x10, 0x20, 0x60, 0 }, /* ...clause 0 second, one shared pad */
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-section-order-authoritative";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, clauses, 2, base, code_len, out);
		bool good = ok && out.size () == 2 &&
			out[0].try_start == (gpointer) (base + 0x10) &&
			out[1].try_start == (gpointer) (base + 0x10) &&
			out[0].clause_index == 1 && out[1].clause_index == 0 &&
			out[0].data.catch_class == CC1 && out[1].data.catch_class == CC0 &&
			out[0].handler_start == (gpointer) (base + 0x60) &&
			out[1].handler_start == (gpointer) (base + 0x60);
		if (!good) {
			printf ("FAIL build-section-order-authoritative: ok=%d size=%zu clauses %d/%d "
			        "(0/1 == a clause_index re-sort crept back in)\n",
			        ok, out.size (), out.size () > 0 ? out[0].clause_index : -1,
			        out.size () > 1 ? out[1].clause_index : -1);
			failures ++;
		} else {
			printf ("ok   build-section-order-authoritative (run published verbatim)\n");
		}
	}

	/*
	 * SIBLING + MULTI-CALL composed: two identical-range sibling pairs at two
	 * DIFFERENT disjoint ranges -> 4 entries -> ACCEPT. Pins that equal-and-
	 * disjoint compose (each range hosts a sibling pair; the two ranges are
	 * disjoint). Four clauses, all CLAUSE_NONE.
	 */
	{
		MonoExceptionClause c4 [4];
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
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-sibling-plus-multicall";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, c4, 4, base, code_len, out);
		bool good = ok && out.size () == 4 &&
			out[0].try_start == out[1].try_start && /* R1 pair shares range */
			out[2].try_start == out[3].try_start && /* R2 pair shares range */
			out[0].try_start != out[2].try_start && /* R1 and R2 disjoint */
			out[0].clause_index == 0 && out[3].clause_index == 3;
		if (!good) {
			printf ("FAIL build-sibling-plus-multicall: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   build-sibling-plus-multicall (4 ei, two sibling pairs)\n");
		}
	}

	/*
	 * Overlapping invoke ranges (the nesting/ordering sanity): [0x10,0x40) and
	 * [0x30,0x60) PARTIALLY overlap (not equal) -> decline (ambiguous first-match,
	 * unsupported nesting). Equal-or-disjoint exempts only EXACTLY equal ranges.
	 */
	expect_build_decline ("build-overlapping-ranges",
		{ { 0x10, 0x30, 0x40, 0 }, { 0x30, 0x30, 0x80, 1 } }, clauses, 2);

	/*
	 * STRICT nesting: [0x10,0x40) fully contains [0x20,0x30). Not equal, so it
	 * still declines - the missed-nesting attack stays covered.
	 */
	expect_build_decline ("build-strict-nesting",
		{ { 0x10, 0x30, 0x40, 0 }, { 0x20, 0x10, 0x80, 1 } }, clauses, 2);

	/*
	 * Touching-but-disjoint ranges [0x10,0x20) and [0x20,0x30) must be ACCEPTED
	 * (half-open ranges that share an endpoint do not overlap).
	 */
	{
		std::vector<MonoLsdaEntry> ents = {
			{ 0x10, 0x10, 0x40, 0 },
			{ 0x20, 0x10, 0x80, 1 },
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-touching-disjoint-ok";
		cases_run ++;
		if (mono::build_ex_info (ents, clauses, 2, base, code_len, out) && out.size () == 2)
			printf ("ok   build-touching-disjoint-ok (2 ei)\n");
		else {
			printf ("FAIL build-touching-disjoint-ok: declined disjoint ranges\n");
			failures ++;
		}
	}
}

/* ------------------------------------------------------ nesting chain cases */

/*
 * EH N1 (doc 21 4): how build_ex_info turns a nesting chain into published
 * entries. A landing pad names every clause that covers it - its own sibling
 * group, then its enclosers innermost-first - and .mono_lsda carries that list in
 * order, one run per invoke range. build_ex_info publishes the run as it stands,
 * so nesting is encoded by same-range entries plus array order.
 *
 * These cases feed such runs directly, and assert the published array: count,
 * per-entry range/handler/flags/clause_index, innermost-first slot order within a
 * range, and equal-or-disjoint still holding.
 *
 * The IL clause table each case passes is now read ONLY for flags and
 * catch_class - the try offsets are set to a plausible nest for readability, but
 * nothing derives containment from them any more. That is the point: an inlined
 * body's IL offsets are meaningless in the caller, and the chain has to come from
 * the pads instead.
 */
static void
cases_mono_lsda_nesting (void)
{
	const std::uint32_t code_len = 0x100;
	std::vector<std::uint8_t> code (code_len, 0);
	const std::uint8_t *base = code.data ();

	/* The base entry's native geometry, shared by every nested case below. */
	const std::uint32_t R_START = 0x10, R_LEN = 0x20, H_OFF = 0x40;

	/*
	 * (1) C# try/catch/finally (doc 21 6.1): inner CATCH(E) nested in outer
	 * FINALLY. IL clause0 = inner catch, clause1 = outer finally. The inner catch's
	 * pad names both, so the gather hands back a two-entry run over one invoke
	 * range. Expect TWO ei in that order, sharing range and handler_start.
	 */
	{
		MonoExceptionClause cl [2];
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
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-try-catch-finally";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 2, base, code_len, out);
		bool good = ok && out.size () == 2 &&
			/* slot 0: the innermost clause, the catch */
			out[0].flags == MONO_EXCEPTION_CLAUSE_NONE &&
			out[0].clause_index == 0 &&
			out[0].data.catch_class == CC0 &&
			out[0].try_start == (gpointer) (base + R_START) &&
			out[0].try_end == (gpointer) (base + R_START + R_LEN) &&
			out[0].handler_start == (gpointer) MINI_ADDR_TO_FTNPTR (base + H_OFF) &&
			/* slot 1: the enclosing finally, SAME range + SAME handler */
			out[1].flags == MONO_EXCEPTION_CLAUSE_FINALLY &&
			out[1].clause_index == 1 &&
			out[1].try_start == out[0].try_start &&
			out[1].try_end == out[0].try_end &&
			out[1].handler_start == out[0].handler_start &&
			out[1].data.handler_end == 0 &&
			out[1].exvar_offset == 0;
		if (!good) {
			printf ("FAIL nest-try-catch-finally: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-try-catch-finally (catch @0, finally @1, same range/handler)\n");
		}
	}

	/*
	 * (2) The mirror - try/finally inside try/catch (doc 21 6.2, the sharpest
	 * case): inner FINALLY nested in outer CATCH(E). The enclosing entry has to
	 * carry the catch's flags NONE + catch_class + clause_index.
	 */
	{
		MonoExceptionClause cl [2];
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
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-try-finally-in-try-catch";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 2, base, code_len, out);
		bool good = ok && out.size () == 2 &&
			out[0].flags == MONO_EXCEPTION_CLAUSE_FINALLY &&
			out[0].clause_index == 0 &&
			out[0].try_start == (gpointer) (base + R_START) &&
			out[0].try_end == (gpointer) (base + R_START + R_LEN) &&
			out[0].handler_start == (gpointer) MINI_ADDR_TO_FTNPTR (base + H_OFF) &&
			out[1].flags == MONO_EXCEPTION_CLAUSE_NONE &&
			out[1].clause_index == 1 &&
			out[1].data.catch_class == CC1 &&
			out[1].try_start == out[0].try_start &&
			out[1].try_end == out[0].try_end &&
			out[1].handler_start == out[0].handler_start;
		if (!good) {
			printf ("FAIL nest-try-finally-in-try-catch: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-try-finally-in-try-catch (finally @0, catch @1)\n");
		}
	}

	/*
	 * (3) The OUTERMOST clause's own pad names only itself - nothing encloses it -
	 * so its run is one entry long and publishes unchanged.
	 */
	{
		MonoExceptionClause cl [2];
		memset (cl, 0, sizeof (cl));
		cl[0].flags = MONO_EXCEPTION_CLAUSE_NONE;
		cl[0].data.catch_class = CC0;
		cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x05;
		cl[1].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
		cl[1].try_offset = 0x10; cl[1].try_len = 0x15; cl[1].handler_offset = 0x25; cl[1].handler_len = 0x05;

		std::vector<MonoLsdaEntry> ents = { { R_START, R_LEN, H_OFF, 1, MONO_EXCEPTION_CLAUSE_FINALLY } };
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-outermost-base-noop";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 2, base, code_len, out);
		bool good = ok && out.size () == 1 &&
			out[0].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[0].clause_index == 1 &&
			out[0].try_start == (gpointer) (base + R_START) &&
			out[0].handler_start == (gpointer) MINI_ADDR_TO_FTNPTR (base + H_OFF);
		if (!good) {
			printf ("FAIL nest-outermost-base-noop: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-outermost-base-noop (1 ei, chain of one)\n");
		}
	}

	/*
	 * (4) Two clauses that do NOT nest yield two independent one-entry runs over
	 * disjoint ranges - the common non-nested method, unchanged.
	 */
	{
		MonoExceptionClause cl [2];
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
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-depth1-unchanged";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 2, base, code_len, out);
		bool good = ok && out.size () == 2 &&
			out[0].clause_index == 0 && out[1].clause_index == 1;
		if (!good) {
			printf ("FAIL nest-depth1-unchanged: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-depth1-unchanged (2 ei, two chains of one)\n");
		}
	}

	/*
	 * (5) Multi-call under nesting: the inner catch's try has TWO protected calls,
	 * so its pad carries TWO invoke ranges and the gather emits its whole clause
	 * list once per range. Expect FOUR ei as two chains of two - each range's inner
	 * catch immediately followed by its enclosing finally over that SAME range.
	 *
	 * The chains are interleaved rather than blocked (all inner, then all outer)
	 * because they are published per range. That is safe precisely because the two
	 * ranges are disjoint: no PC matches both, so the runtime's flat walk only ever
	 * sees one chain, in order.
	 */
	{
		MonoExceptionClause cl [2];
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
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-multicall-chain-per-range";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 2, base, code_len, out);
		bool good = ok && out.size () == 4 &&
			/* range 1's chain: inner catch then its encloser */
			out[0].flags == MONO_EXCEPTION_CLAUSE_NONE && out[0].clause_index == 0 &&
			out[1].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[1].clause_index == 1 &&
			out[0].try_start == (gpointer) (base + 0x10) &&
			out[1].try_start == out[0].try_start && out[1].try_end == out[0].try_end &&
			out[1].handler_start == out[0].handler_start &&
			/* range 2's chain, over its OWN range */
			out[2].flags == MONO_EXCEPTION_CLAUSE_NONE && out[2].clause_index == 0 &&
			out[3].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[3].clause_index == 1 &&
			out[2].try_start == (gpointer) (base + 0x28) &&
			out[3].try_start == out[2].try_start && out[3].try_end == out[2].try_end &&
			out[3].handler_start == out[2].handler_start;
		if (!good) {
			printf ("FAIL nest-multicall-chain-per-range: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-multicall-chain-per-range (4 ei: two chains of two)\n");
		}
	}

	/*
	 * (6) SIBLINGS are not nesting. try { } catch(A) catch(B) share the identical
	 * protected region and one landing pad, so the pad names both and neither is an
	 * encloser of the other. Expect exactly TWO ei, in declaration order.
	 */
	{
		MonoExceptionClause cl [2];
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
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-siblings-in-declaration-order";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 2, base, code_len, out);
		bool good = ok && out.size () == 2 &&
			out[0].clause_index == 0 && out[1].clause_index == 1 &&
			out[0].data.catch_class == CC0 && out[1].data.catch_class == CC1;
		if (!good) {
			printf ("FAIL nest-siblings-in-declaration-order: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-siblings-in-declaration-order (2 ei, A before B)\n");
		}
	}

	/*
	 * (7) Two clauses whose IL try regions CROSS are published as the two
	 * independent runs the pads describe. Nothing here reads their try offsets, so
	 * a crossing IL shape is not this stage's problem - the translator's gate
	 * declines it - and the native ranges are disjoint, so the array is accepted.
	 * (The native-range crossing decline is covered by build-overlapping-ranges /
	 * build-strict-nesting above.)
	 */
	{
		MonoExceptionClause cl [2];
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
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-crossing-il-not-consulted";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 2, base, code_len, out);
		bool good = ok && out.size () == 2 &&
			out[0].clause_index == 0 && out[1].clause_index == 1;
		if (!good) {
			printf ("FAIL nest-crossing-il-not-consulted: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-crossing-il-not-consulted (2 ei)\n");
		}
	}

	/*
	 * (8) SIBLING GROUP ENCLOSED BY A FINALLY - the enclosing entry appears EXACTLY
	 * ONCE, not once per sibling (the tier-1 double-run finally). try { try {throw}
	 * catch(A) catch(B) } finally {}: clause0/clause1 are the inner sibling catches,
	 * clause2 the outer finally. All three sit on the ONE inner landing pad, so the
	 * run is [A, B, finally] - the finally is named once by the pad, which is what
	 * makes the duplicate structurally impossible rather than deduplicated after the
	 * fact. Two identical finally ei would make pass-2's first-match-and-continue
	 * run it twice when an exception propagates past both siblings (an ECMA-335
	 * §12.4.2 violation). Expect exactly THREE ei - NOT four.
	 */
	{
		MonoExceptionClause cl [3];
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
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-sibling-group-in-finally-once";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 3, base, code_len, out);
		bool good = ok && out.size () == 3 &&
			/* slots 0,1: the two sibling catches, shared range + landing pad */
			out[0].flags == MONO_EXCEPTION_CLAUSE_NONE && out[0].clause_index == 0 &&
			out[1].flags == MONO_EXCEPTION_CLAUSE_NONE && out[1].clause_index == 1 &&
			out[0].try_start == (gpointer) (base + 0x10) &&
			out[0].try_end == (gpointer) (base + 0x20) &&
			out[1].try_start == out[0].try_start && out[1].try_end == out[0].try_end &&
			out[0].handler_start == out[1].handler_start &&
			/* slot 2: the ONE enclosing finally over the shared range/landing pad */
			out[2].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[2].clause_index == 2 &&
			out[2].try_start == out[0].try_start && out[2].try_end == out[0].try_end &&
			out[2].handler_start == out[0].handler_start;
		if (!good) {
			printf ("FAIL nest-sibling-group-in-finally-once: ok=%d size=%zu (expected 3, "
			        "size 4 == the double-published finally bug)\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-sibling-group-in-finally-once (3 ei: 2 sibling catches, finally once)\n");
		}
	}

	/*
	 * (9) The same sibling group with the inner try spanning TWO DISTINCT invoke
	 * ranges: the enclosing finally must appear once PER RANGE, not once overall.
	 * The pad carries both ranges and the gather emits its clause list for each, so
	 * expect SIX ei as two chains of three, each over its own range/landing pad.
	 */
	{
		MonoExceptionClause cl [3];
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
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-sibling-group-multirange-one-per-range";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 3, base, code_len, out);
		bool good = ok && out.size () == 6 &&
			/* R1's chain */
			out[0].clause_index == 0 && out[1].clause_index == 1 &&
			out[2].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[2].clause_index == 2 &&
			out[0].try_start == (gpointer) (base + 0x10) &&
			out[2].try_start == out[0].try_start && out[2].try_end == out[0].try_end &&
			out[2].handler_start == out[0].handler_start &&
			/* R2's chain, over its OWN range and pad */
			out[3].clause_index == 0 && out[4].clause_index == 1 &&
			out[5].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[5].clause_index == 2 &&
			out[3].try_start == (gpointer) (base + 0x28) &&
			out[5].try_start == out[3].try_start && out[5].try_end == out[3].try_end &&
			out[5].handler_start == out[3].handler_start &&
			out[2].try_start != out[5].try_start;
		if (!good) {
			printf ("FAIL nest-sibling-group-multirange-one-per-range: ok=%d size=%zu "
			        "(expected 6: two chains of three)\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-sibling-group-multirange-one-per-range (6 ei: one finally per range)\n");
		}
	}

	/*
	 * (10) DEPTH-3 try/finally x3 - encloser ORDER at depth >= 3 (doc 21 4.1, EH N6).
	 * clause0 (inner) in clause1 (middle) in clause2 (outer), all finallys, all named
	 * by the inner pad. Order is load-bearing: pass-2 resumes at the running clause's
	 * ARRAY slot + 1, so [inner@0, middle@1, outer@2] is what makes the runtime run
	 * the finallys inner-to-outer. A DESCENDING chain would give [inner, outer,
	 * middle] and run them out of order.
	 */
	{
		MonoExceptionClause cl [3];
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
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-depth3-finally-ascending-order";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 3, base, code_len, out);
		bool good = ok && out.size () == 3 &&
			out[0].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[0].clause_index == 0 &&
			out[0].try_start == (gpointer) (base + R_START) &&
			out[0].try_end == (gpointer) (base + R_START + R_LEN) &&
			out[0].handler_start == (gpointer) MINI_ADDR_TO_FTNPTR (base + H_OFF) &&
			out[1].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[1].clause_index == 1 &&
			out[2].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[2].clause_index == 2 &&
			/* every encloser shares the inner clause's range + landing pad */
			out[1].try_start == out[0].try_start && out[1].try_end == out[0].try_end &&
			out[1].handler_start == out[0].handler_start &&
			out[2].try_start == out[0].try_start && out[2].try_end == out[0].try_end &&
			out[2].handler_start == out[0].handler_start;
		if (!good) {
			printf ("FAIL nest-depth3-finally-ascending-order: ok=%d size=%zu "
			        "(expected 3 ei, clause_index 0,1,2 innermost-first)\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-depth3-finally-ascending-order (3 ei: inner@0, middle@1, outer@2)\n");
		}
	}

	/*
	 * (11) DEPTH-4 - the same ordering over THREE enclosers of one clause.
	 */
	{
		MonoExceptionClause cl [4];
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
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-depth4-finally-ascending-order";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 4, base, code_len, out);
		bool good = ok && out.size () == 4 &&
			out[0].clause_index == 0 && out[1].clause_index == 1 &&
			out[2].clause_index == 2 && out[3].clause_index == 3 &&
			out[0].flags == MONO_EXCEPTION_CLAUSE_FINALLY &&
			out[3].flags == MONO_EXCEPTION_CLAUSE_FINALLY &&
			out[1].try_start == out[0].try_start && out[1].handler_start == out[0].handler_start &&
			out[3].try_start == out[0].try_start && out[3].handler_start == out[0].handler_start;
		if (!good) {
			printf ("FAIL nest-depth4-finally-ascending-order: ok=%d size=%zu "
			        "(expected 4 ei, clause_index 0,1,2,3)\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-depth4-finally-ascending-order (4 ei: 0,1,2,3 innermost-first)\n");
		}
	}

	/*
	 * (12) A sibling catch group enclosed by TWO finallys - siblings and depth >= 3
	 * composing. One pad names all four clauses, so the run is [A, B, middle, outer]
	 * and each encloser appears exactly once, after both siblings, in nesting order.
	 */
	{
		MonoExceptionClause cl [4];
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
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-depth3-sibling-ascending";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 4, base, code_len, out);
		bool good = ok && out.size () == 4 &&
			/* slots 0,1: the two sibling catches over the shared range/pad */
			out[0].flags == MONO_EXCEPTION_CLAUSE_NONE && out[0].clause_index == 0 &&
			out[1].flags == MONO_EXCEPTION_CLAUSE_NONE && out[1].clause_index == 1 &&
			out[0].try_start == out[1].try_start && out[0].handler_start == out[1].handler_start &&
			/* slots 2,3: the two enclosing finallys, once each, innermost-first */
			out[2].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[2].clause_index == 2 &&
			out[3].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[3].clause_index == 3 &&
			out[2].try_start == out[0].try_start && out[2].handler_start == out[0].handler_start &&
			out[3].try_start == out[0].try_start && out[3].handler_start == out[0].handler_start;
		if (!good) {
			printf ("FAIL nest-depth3-sibling-ascending: ok=%d size=%zu "
			        "(expected 4: 2 sibling catches + 2 enclosers once each)\n",
			        ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-depth3-sibling-ascending (4 ei: 2 siblings, 2 enclosers ascending)\n");
		}
	}
}
/* ------------------------------------------------------------ guard cases */

/*
 * The thread-abort guard entries build_ex_info appends for a FINALLY clause,
 * one per PC range the recovery found the handler body occupying.
 *
 * What these pin down is that a guard entry is inert for dispatch (empty try
 * range) while still carrying the handler extent and exvar the runtime's guard
 * reads, that a clause gets one entry per body copy rather than a single span
 * over all of them, and that an extent we cannot state declines the method.
 */
static void
cases_mono_lsda_finally_guards (void)
{
	const std::uint32_t code_len = 0x100;
	std::vector<std::uint8_t> code (code_len, 0);
	const std::uint8_t *base = code.data ();

	MonoExceptionClause clauses [2];
	memset (clauses, 0, sizeof (clauses));
	clauses[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
	clauses[1].flags = MONO_EXCEPTION_CLAUSE_NONE;
	clauses[1].data.catch_class = CC0;

	/*
	 * A finally whose try region has no protected call publishes no dispatch
	 * entry at all - the shape that left the guard uninstallable. The guard
	 * entry must appear anyway, which is the whole point of it being separate.
	 */
	{
		std::vector<MonoLsdaEntry> ents;
		std::vector<MonoFinallyGuard> guards = { { 0, 0x20, 0x30, -24 } };
		std::vector<MonoJitExceptionInfo> out;
		current_case = "guard-no-dispatch-entry";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, clauses, 2, base, code_len, out, guards);
		bool good = ok && out.size () == 1 &&
			out[0].flags == MONO_EXCEPTION_CLAUSE_FINALLY &&
			out[0].clause_index == 0 &&
			/* Empty try range: is_address_protected () is false for every PC. */
			out[0].try_start == (gpointer) base &&
			out[0].try_end == (gpointer) base &&
			out[0].handler_start == (gpointer) (base + 0x20) &&
			out[0].data.handler_end == (gpointer) (base + 0x30) &&
			out[0].exvar_offset == -24;
		if (!good) {
			printf ("FAIL guard-no-dispatch-entry: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   guard-no-dispatch-entry (1 guard ei)\n");
		}
	}

	/*
	 * A duplicated body: two ranges for one clause, published as two entries
	 * with the same exvar. A single span [0x20,0x90) would have covered the
	 * unrelated code between them and made the guard match PCs that are not in
	 * the finally at all.
	 */
	{
		std::vector<MonoLsdaEntry> ents = {
			{ 0x10, 0x08, 0x40, 1 },
		};
		std::vector<MonoFinallyGuard> guards = {
			{ 0, 0x20, 0x30, -24 },
			{ 0, 0x80, 0x90, -24 },
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "guard-duplicated-body";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, clauses, 2, base, code_len, out, guards);
		bool good = ok && out.size () == 3 &&
			/* The dispatch entry keeps slot 0; guards are appended after it. */
			out[0].flags == MONO_EXCEPTION_CLAUSE_NONE &&
			out[1].handler_start == (gpointer) (base + 0x20) &&
			out[1].data.handler_end == (gpointer) (base + 0x30) &&
			out[2].handler_start == (gpointer) (base + 0x80) &&
			out[2].data.handler_end == (gpointer) (base + 0x90) &&
			out[1].exvar_offset == -24 && out[2].exvar_offset == -24;
		if (!good) {
			printf ("FAIL guard-duplicated-body: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   guard-duplicated-body (2 guard ei)\n");
		}
	}

	/*
	 * Two clauses each with their own body, to pin that a guard entry is keyed to
	 * the right clause when there is more than one to confuse it with.
	 */
	{
		MonoExceptionClause two_finally [2];
		memset (two_finally, 0, sizeof (two_finally));
		two_finally[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
		two_finally[1].flags = MONO_EXCEPTION_CLAUSE_FINALLY;

		std::vector<MonoLsdaEntry> ents;
		std::vector<MonoFinallyGuard> guards = {
			{ 0, 0x20, 0x30, -24 },
			{ 1, 0x60, 0x70, -32 },
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "guard-two-clauses";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, two_finally, 2, base, code_len, out, guards);
		bool good = ok && out.size () == 2 &&
			out[0].clause_index == 0 && out[0].exvar_offset == -24 &&
			out[0].handler_start == (gpointer) (base + 0x20) &&
			out[1].clause_index == 1 && out[1].exvar_offset == -32 &&
			out[1].handler_start == (gpointer) (base + 0x60);
		if (!good) {
			printf ("FAIL guard-two-clauses: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   guard-two-clauses\n");
		}
	}

	/* The base register is per guard, not per method - two copies of one body
	 * can be homed against different registers. */
	{
		std::vector<MonoLsdaEntry> ents;
		/* 4/5 are AMD64_RSP/AMD64_RBP; this only checks that the byte survives. */
		std::vector<MonoFinallyGuard> guards = {
			{ 0, 0x20, 0x30, -24, 4 },
			{ 0, 0x80, 0x90, -24, 5 },
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "guard-base-reg";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, clauses, 2, base, code_len, out, guards);
		bool good = ok && out.size () == 2 &&
			out[0].exvar_offset == -24 && out[0].exvar_base_reg == AMD64_RSP &&
			out[1].exvar_offset == -24 && out[1].exvar_base_reg == AMD64_RBP;
		if (!good) {
			printf ("FAIL guard-base-reg: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   guard-base-reg\n");
		}
	}

	/* A catch-only method passes no guards and is unaffected. */
	{
		std::vector<MonoLsdaEntry> ents = { { 0x10, 0x08, 0x40, 1 } };
		std::vector<MonoJitExceptionInfo> out;
		current_case = "guard-absent-for-catch";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, clauses, 2, base, code_len, out);
		/* data is a union - a catch entry's slot holds catch_class, not an extent. */
		bool good = ok && out.size () == 1 &&
			out[0].flags == MONO_EXCEPTION_CLAUSE_NONE &&
			out[0].data.catch_class == CC0 &&
			out[0].exvar_offset == 0;
		if (!good) {
			printf ("FAIL guard-absent-for-catch: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   guard-absent-for-catch\n");
		}
	}
}

/* ------------------------------------------------------------ publish cases */

/*
 * publish_mono_lsda is the thin MonoCompile-aware wrapper over build_ex_info:
 * it joins cfg->header->clauses[] into the entries, and on success allocates
 * cfg->llvm_ex_info[] from cfg->mempool and sets cfg->llvm_ex_info_len; on
 * decline it must leave cfg entirely untouched (CAP-EH-0: the caller falls
 * back to the classic JIT without a half-written cfg). These cases drive it
 * with a real MonoMethodHeader/MonoCompile pair instead of calling
 * build_ex_info directly, pinning that wiring.
 */
static void
cases_mono_lsda_publish (void)
{
	const std::uint32_t code_len = 0x100;
	std::vector<std::uint8_t> code (code_len, 0);
	const std::uint8_t *base = code.data ();

	MonoExceptionClause clauses [2];
	memset (clauses, 0, sizeof (clauses));
	clauses[0].flags = MONO_EXCEPTION_CLAUSE_NONE;
	clauses[0].data.catch_class = CC0;
	clauses[1].flags = MONO_EXCEPTION_CLAUSE_NONE;
	clauses[1].data.catch_class = CC1;

	MonoMethodHeader header;
	memset (&header, 0, sizeof (header));
	header.num_clauses = 2;
	header.clauses = clauses;

	MonoMemPool *pool = mono_mempool_new ();

	/*
	 * (1) Happy path: two valid disjoint entries against a real cfg + clause table
	 * publish exactly what build_ex_info would produce for the same inputs
	 * (cross-checked against cases_mono_lsda_build's equivalent vectors).
	 */
	{
		MonoCompile cfg;
		memset (&cfg, 0, sizeof (cfg));
		cfg.mempool = pool;
		cfg.header = &header;

		std::vector<MonoExceptionClause> table (clauses, clauses + 2);
		std::vector<MonoLsdaEntry> ents = {
			{ 0x10, 0x20, 0x40, 0 }, /* [0x10,0x30) -> handler 0x40, clause 0 */
			{ 0x50, 0x10, 0x80, 1 }, /* [0x50,0x60) -> handler 0x80, clause 1 */
		};
		current_case = "publish-valid-two-clause";
		cases_run ++;
		bool ok = mono::publish_mono_lsda (&cfg, table, ents, base, code_len);
		bool good = ok && cfg.llvm_ex_info_len == 2 && cfg.llvm_ex_info != nullptr;
		if (good) {
			const MonoJitExceptionInfo &e0 = cfg.llvm_ex_info [0];
			const MonoJitExceptionInfo &e1 = cfg.llvm_ex_info [1];
			good =
				e0.clause_index == 0 &&
				e0.try_start == (gpointer) (base + 0x10) &&
				e0.try_end == (gpointer) (base + 0x30) &&
				e0.handler_start == (gpointer) (base + 0x40) &&
				e0.data.catch_class == CC0 &&
				e1.clause_index == 1 &&
				e1.try_start == (gpointer) (base + 0x50) &&
				e1.try_end == (gpointer) (base + 0x60) &&
				e1.handler_start == (gpointer) (base + 0x80) &&
				e1.data.catch_class == CC1;
		}
		if (!good) {
			printf ("FAIL publish-valid-two-clause: ok=%d len=%u ptr=%p\n",
			        ok, cfg.llvm_ex_info_len, (void*) cfg.llvm_ex_info);
			failures ++;
		} else {
			printf ("ok   publish-valid-two-clause (cfg.llvm_ex_info_len=2)\n");
		}
	}

	/*
	 * (2) An EMPTY clause table with no entries is the vacuous accept case - and
	 * since the built array is empty, the n==0 branch must skip the mempool
	 * allocation entirely, leaving llvm_ex_info null.
	 */
	{
		MonoCompile cfg;
		memset (&cfg, 0, sizeof (cfg));
		cfg.mempool = pool;
		cfg.header = nullptr;

		std::vector<MonoExceptionClause> table;
		std::vector<MonoLsdaEntry> ents;
		current_case = "publish-empty-table-empty-entries";
		cases_run ++;
		bool ok = mono::publish_mono_lsda (&cfg, table, ents, base, code_len);
		bool good = ok && cfg.llvm_ex_info == nullptr && cfg.llvm_ex_info_len == 0;
		if (!good) {
			printf ("FAIL publish-empty-table-empty-entries: ok=%d ptr=%p len=%u\n",
			        ok, (void*) cfg.llvm_ex_info, cfg.llvm_ex_info_len);
			failures ++;
		} else {
			printf ("ok   publish-empty-table-empty-entries (llvm_ex_info_len=0)\n");
		}
	}

	mono_mempool_destroy (pool);
}

/* ------------------------------------------------------------ entry point */

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_ehtable_main (void);

int
test_llvm_ehtable_main (void)
{
	failures = 0;
	cases_run = 0;

	setvbuf (stdout, nullptr, _IOLBF, 0);
	install_crash_handler ();

	cases_mono_lsda_parse ();
	cases_mono_lsda_build ();
	cases_mono_lsda_nesting ();
	cases_mono_lsda_finally_guards ();
	cases_mono_lsda_publish ();

	printf ("%d cases run, %d failed\n", cases_run, failures);
	return failures ? 1 : 0;
}

#else /* !ENABLE_LLVM */

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_ehtable_main (void);

int
test_llvm_ehtable_main (void)
{
	return 0;
}

#endif /* ENABLE_LLVM */
