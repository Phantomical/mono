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
	 * SIBLING ORDER is authoritative on clause_index, NOT on section order. The
	 * gather pass records one entry per landing-pad TypeId, and LLVM hands those
	 * back in REVERSE of the emitted clause order, so a sibling group arrives with
	 * DESCENDING clause_index. build_ex_info must reorder each shared range to
	 * ASCENDING clause_index (declaration order) so the runtime's first-isinst-
	 * match picks the earlier-declared catch - otherwise `catch(Derived)
	 * catch(Base)` lets Base swallow a Derived throw. Feed the reversed order and
	 * assert the published array is ascending within the range.
	 */
	{
		std::vector<MonoLsdaEntry> ents = {
			{ 0x10, 0x20, 0x90, 1 }, /* catch B first in the section (clause 1) */
			{ 0x10, 0x20, 0x60, 0 }, /* catch A second (clause 0) */
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "build-sibling-reversed-order";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, clauses, 2, base, code_len, out);
		bool good = ok && out.size () == 2 &&
			out[0].try_start == (gpointer) (base + 0x10) &&
			out[1].try_start == (gpointer) (base + 0x10) &&
			out[0].clause_index == 0 && out[1].clause_index == 1 &&
			out[0].data.catch_class == CC0 && out[1].data.catch_class == CC1 &&
			out[0].handler_start == (gpointer) (base + 0x60) &&
			out[1].handler_start == (gpointer) (base + 0x90);
		if (!good) {
			printf ("FAIL build-sibling-reversed-order: ok=%d size=%zu clauses %d/%d\n",
			        ok, out.size (), out.size () > 0 ? out[0].clause_index : -1,
			        out.size () > 1 ? out[1].clause_index : -1);
			failures ++;
		} else {
			printf ("ok   build-sibling-reversed-order (reordered to ascending clause_index)\n");
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

/* ------------------------------------------------------ nesting synthesis cases */

/*
 * EH N1 (doc 21 4): build_ex_info's nesting synthesis. For each base entry whose
 * innermost clause is `c`, one extra ei is APPENDED per enclosing clause `j`
 * (clause c strictly try-contained in clause j), copying the base's EXACT range
 * and handler_start and overriding only j's flags/catch_class/clause_index. These
 * cases feed a SYNTHETIC nested IL clause table (try_offset/try_len/handler_offset
 * set so the containment predicate fires) plus a base `.mono_lsda`-derived tuple,
 * and assert the synthesised ei array: count, per-entry range/handler/flags/
 * clause_index, base-before-synthesised slot order, and equal-or-disjoint still
 * holding. This is the offline exercise of a synthesis the live gate still
 * declines (N1 is runtime-inert), the same OFFLINE byte-driven style as above.
 *
 * The IL clause geometry mirrors doc 21 6.1 / 6.2: an inner clause with try
 * [0x10,0x20) / handler at 0x20, and an outer clause whose try [0x10,0x25) spans
 * {inner try + inner handler} with its own handler at 0x25. These are IL offsets
 * (only their ordering matters to clause_encloses); the NATIVE range/handler come
 * from the base entry independently.
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
	 * FINALLY. IL clause0 = inner catch (try [0x10,0x20), handler 0x20); clause1 =
	 * outer finally (try [0x10,0x25) covering the inner try+catch, handler 0x25).
	 * The gather hands a single base entry over the inner catch (clause0). Expect
	 * TWO ei: the base inner-catch (slot 0) AND the synthesised outer-finally (slot
	 * 1) over the SAME native range with the SAME handler_start, carrying the
	 * finally's flags/clause_index - base-before-synthesised.
	 */
	{
		MonoExceptionClause cl [2];
		memset (cl, 0, sizeof (cl));
		cl[0].flags = MONO_EXCEPTION_CLAUSE_NONE;    /* inner catch */
		cl[0].data.catch_class = CC0;
		cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x05;
		cl[1].flags = MONO_EXCEPTION_CLAUSE_FINALLY; /* outer finally */
		cl[1].try_offset = 0x10; cl[1].try_len = 0x15; cl[1].handler_offset = 0x25; cl[1].handler_len = 0x05;

		std::vector<MonoLsdaEntry> ents = { { R_START, R_LEN, H_OFF, 0, MONO_EXCEPTION_CLAUSE_NONE } };
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-try-catch-finally";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 2, base, code_len, out);
		bool good = ok && out.size () == 2 &&
			/* slot 0: the base inner catch */
			out[0].flags == MONO_EXCEPTION_CLAUSE_NONE &&
			out[0].clause_index == 0 &&
			out[0].data.catch_class == CC0 &&
			out[0].try_start == (gpointer) (base + R_START) &&
			out[0].try_end == (gpointer) (base + R_START + R_LEN) &&
			out[0].handler_start == (gpointer) MINI_ADDR_TO_FTNPTR (base + H_OFF) &&
			/* slot 1: the SYNTHESISED outer finally, SAME range + SAME handler */
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
			printf ("ok   nest-try-catch-finally (base catch @0, synth finally @1, same range/handler)\n");
		}
	}

	/*
	 * (2) The mirror - try/finally inside try/catch (doc 21 6.2, the sharpest
	 * case): inner FINALLY nested in outer CATCH(E). IL clause0 = inner finally
	 * (try [0x10,0x20), handler 0x20); clause1 = outer catch (try [0x10,0x25),
	 * handler 0x25). The base entry is over the inner finally (clause0). Expect the
	 * base inner-finally (slot 0) AND the synthesised outer-catch (slot 1) - carrying
	 * the catch's flags NONE + catch_class + clause_index - over the same range/handler.
	 */
	{
		MonoExceptionClause cl [2];
		memset (cl, 0, sizeof (cl));
		cl[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY; /* inner finally */
		cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x05;
		cl[1].flags = MONO_EXCEPTION_CLAUSE_NONE;    /* outer catch */
		cl[1].data.catch_class = CC1;
		cl[1].try_offset = 0x10; cl[1].try_len = 0x15; cl[1].handler_offset = 0x25; cl[1].handler_len = 0x05;

		std::vector<MonoLsdaEntry> ents = { { R_START, R_LEN, H_OFF, 0, MONO_EXCEPTION_CLAUSE_FINALLY } };
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
			printf ("ok   nest-try-finally-in-try-catch (base finally @0, synth catch @1)\n");
		}
	}

	/*
	 * (3) Synthesis is a NO-OP when the base names the OUTERMOST clause. Same
	 * nested clause table as (1), but the base entry is over the outer finally
	 * (clause1), which has no enclosing clause (nested_in[1] is empty). Output is
	 * the single base entry, unchanged - proving no spurious synthesis and no
	 * regression to the landed finally publish.
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
			out[0].flags == MONO_EXCEPTION_CLAUSE_FINALLY &&
			out[0].clause_index == 1;
		if (!good) {
			printf ("FAIL nest-outermost-base-noop: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-outermost-base-noop (1 ei, no synthesis)\n");
		}
	}

	/*
	 * (4) DEPTH-1 (non-nested) input is unchanged: two disjoint clauses that do NOT
	 * nest (clause0 try [0x10,0x20)/handler 0x20; clause1 try [0x40,0x50)/handler
	 * 0x50 - a later, disjoint try) yield exactly their two base entries, no
	 * synthesis. Proves the synthesis no-ops for the common non-nested method the
	 * landed catch/finally path already handles.
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
			printf ("ok   nest-depth1-unchanged (2 ei, no synthesis)\n");
		}
	}

	/*
	 * (5) Multi-call under nesting: the inner catch's try has TWO protected calls,
	 * so the gather hands TWO disjoint base entries over clause0, each nested in the
	 * outer finally (clause1). Expect FOUR ei: two base entries (slots 0,1) then two
	 * synthesised finally entries (slots 2,3), each synth copying its OWN base's
	 * range - so the base block precedes the synth block (base-before-synthesised
	 * across multiple calls) and equal-or-disjoint still holds (each synth equals
	 * its base; the two base ranges are disjoint).
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
			{ 0x10, 0x08, 0x40, 0, MONO_EXCEPTION_CLAUSE_NONE }, /* call 1 */
			{ 0x28, 0x08, 0x40, 0, MONO_EXCEPTION_CLAUSE_NONE }, /* call 2, disjoint */
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-multicall-base-before-synth";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 2, base, code_len, out);
		bool good = ok && out.size () == 4 &&
			/* the two base inner-catch entries occupy the lower slots */
			out[0].flags == MONO_EXCEPTION_CLAUSE_NONE && out[0].clause_index == 0 &&
			out[1].flags == MONO_EXCEPTION_CLAUSE_NONE && out[1].clause_index == 0 &&
			out[0].try_start == (gpointer) (base + 0x10) &&
			out[1].try_start == (gpointer) (base + 0x28) &&
			/* the two synthesised finally entries occupy the higher slots */
			out[2].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[2].clause_index == 1 &&
			out[3].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[3].clause_index == 1 &&
			/* each synth copies its OWN base's range + handler */
			out[2].try_start == out[0].try_start && out[2].try_end == out[0].try_end &&
			out[2].handler_start == out[0].handler_start &&
			out[3].try_start == out[1].try_start && out[3].try_end == out[1].try_end &&
			out[3].handler_start == out[1].handler_start;
		if (!good) {
			printf ("FAIL nest-multicall-base-before-synth: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-multicall-base-before-synth (4 ei: 2 base then 2 synth)\n");
		}
	}

	/*
	 * (6) SIBLINGS are NOT synthesised. try { } catch(A) catch(B) share the
	 * identical protected region (equal try_offset AND try_len) and differ only in
	 * handler - the sibling exemption in clause_encloses must keep nested_in empty
	 * so no spurious co-sibling duplicate is appended. Feed both sibling base
	 * entries over the shared range; expect exactly TWO ei (the siblings), no third
	 * synthesised entry. This is the property that keeps N1 runtime-inert for the
	 * sibling-catch methods the gate admits today.
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
			{ 0x10, 0x10, 0x90, 1, MONO_EXCEPTION_CLAUSE_NONE },
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-siblings-not-synthesised";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 2, base, code_len, out);
		bool good = ok && out.size () == 2 &&
			out[0].clause_index == 0 && out[1].clause_index == 1;
		if (!good) {
			printf ("FAIL nest-siblings-not-synthesised: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-siblings-not-synthesised (2 ei, no co-sibling duplicate)\n");
		}
	}

	/*
	 * (7) An enclosing clause of an UNREPRESENTABLE kind declines (CAP-EH-0, doc 21
	 * 7 item 3). clause0 = inner catch nested in clause1 = a FILTER (the kind the
	 * gate forbids). The base inner-catch entry is well-formed, but synthesising its
	 * enclosing FILTER entry is impossible, so the whole array declines rather than
	 * publish a partial one.
	 */
	{
		MonoExceptionClause cl [2];
		memset (cl, 0, sizeof (cl));
		cl[0].flags = MONO_EXCEPTION_CLAUSE_NONE;
		cl[0].data.catch_class = CC0;
		cl[0].try_offset = 0x10; cl[0].try_len = 0x10; cl[0].handler_offset = 0x20; cl[0].handler_len = 0x05;
		cl[1].flags = MONO_EXCEPTION_CLAUSE_FILTER;
		cl[1].try_offset = 0x10; cl[1].try_len = 0x15; cl[1].handler_offset = 0x25; cl[1].handler_len = 0x05;

		std::vector<MonoLsdaEntry> ents = { { R_START, R_LEN, H_OFF, 0, MONO_EXCEPTION_CLAUSE_NONE } };
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-enclosing-filter-declines";
		cases_run ++;
		if (mono::build_ex_info (ents, cl, 2, base, code_len, out)) {
			printf ("FAIL nest-enclosing-filter-declines: accepted an unrepresentable enclosing kind\n");
			failures ++;
		} else {
			printf ("ok   nest-enclosing-filter-declines (declined)\n");
		}
	}

	/*
	 * (8) A genuinely CROSSING clause table (neither containment nor siblings) is
	 * not folded into nested_in: clause0 try [0x10,0x30) and clause1 try [0x20,0x40)
	 * partially overlap in IL but neither encloses the other, so clause_encloses is
	 * false both ways and no synthesis happens. The base entries are disjoint native
	 * ranges, so the array is ACCEPTED as two plain base entries - the crossing is an
	 * IL-shape concern the translator gate declines, not something the synthesis
	 * invents coverage for. (The native-range crossing decline is covered by
	 * build-overlapping-ranges / build-strict-nesting above.)
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
		current_case = "nest-crossing-no-synthesis";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 2, base, code_len, out);
		bool good = ok && out.size () == 2 &&
			out[0].clause_index == 0 && out[1].clause_index == 1;
		if (!good) {
			printf ("FAIL nest-crossing-no-synthesis: ok=%d size=%zu\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-crossing-no-synthesis (2 ei, no synthesis for crossing clauses)\n");
		}
	}

	/*
	 * (9) SIBLING GROUP ENCLOSED BY A FINALLY - the enclosing entry is synthesised
	 * EXACTLY ONCE, not once per sibling (the fix for the tier-1 double-run finally).
	 * try { try {throw} catch(A) catch(B) } finally {}: clause0 = inner catch A,
	 * clause1 = inner catch B (sibling of 0 - identical try_offset AND try_len),
	 * clause2 = outer finally spanning both inner handlers. The gather publishes TWO
	 * base entries over the SAME native range sharing the ONE inner landing pad. Each
	 * sibling base would, un-de-duplicated, re-synthesise the SAME enclosing finally
	 * over that range, giving TWO identical finally ei; pass-2's first-match-and-
	 * continue then runs the finally twice when an exception propagates past both
	 * siblings (ECMA-335 §12.4.2 violation). De-dup by (range, enclosing clause_index)
	 * collapses them to ONE. Expect exactly THREE ei: the two sibling base catches
	 * (slots 0,1) then the SINGLE synthesised finally (slot 2) - NOT four.
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

		/* Two sibling base entries: SAME range, SAME (shared) landing pad 0x40. */
		std::vector<MonoLsdaEntry> ents = {
			{ 0x10, 0x10, 0x40, 0, MONO_EXCEPTION_CLAUSE_NONE },
			{ 0x10, 0x10, 0x40, 1, MONO_EXCEPTION_CLAUSE_NONE },
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-sibling-group-in-finally-once";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 3, base, code_len, out);
		bool good = ok && out.size () == 3 &&
			/* slots 0,1: the two sibling base catches, shared range + landing pad */
			out[0].flags == MONO_EXCEPTION_CLAUSE_NONE && out[0].clause_index == 0 &&
			out[1].flags == MONO_EXCEPTION_CLAUSE_NONE && out[1].clause_index == 1 &&
			out[0].try_start == (gpointer) (base + 0x10) &&
			out[0].try_end == (gpointer) (base + 0x20) &&
			out[1].try_start == out[0].try_start && out[1].try_end == out[0].try_end &&
			out[0].handler_start == out[1].handler_start &&
			/* slot 2: the ONE synthesised finally over the shared range/landing pad */
			out[2].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[2].clause_index == 2 &&
			out[2].try_start == out[0].try_start && out[2].try_end == out[0].try_end &&
			out[2].handler_start == out[0].handler_start;
		if (!good) {
			printf ("FAIL nest-sibling-group-in-finally-once: ok=%d size=%zu (expected 3, "
			        "size 4 == the double-synthesised finally bug)\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-sibling-group-in-finally-once (3 ei: 2 sibling catches, finally synthesised once)\n");
		}
	}

	/*
	 * (10) SIBLING GROUP with the inner try spanning TWO DISTINCT invoke ranges,
	 * enclosed by an outer finally: the de-dup collapses siblings WITHIN a range but
	 * keeps one enclosing entry PER DISTINCT range (it must not over-dedup across
	 * invoke ranges). Same clause table as (9); the gather hands FOUR base entries -
	 * two sibling pairs, one pair at range R1 [0x10,0x18), one at R2 [0x28,0x30),
	 * each pair sharing its own landing pad. Expect SIX ei: the four sibling base
	 * catches (slots 0..3) then TWO synthesised finallys (slots 4,5) - one per
	 * distinct range, each over its own range/landing pad - NOT one (over-deduped)
	 * and NOT four (un-deduped).
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
			{ 0x10, 0x08, 0x40, 0, MONO_EXCEPTION_CLAUSE_NONE }, /* R1 sibling A */
			{ 0x10, 0x08, 0x40, 1, MONO_EXCEPTION_CLAUSE_NONE }, /* R1 sibling B (shared pad) */
			{ 0x28, 0x08, 0x50, 0, MONO_EXCEPTION_CLAUSE_NONE }, /* R2 sibling A */
			{ 0x28, 0x08, 0x50, 1, MONO_EXCEPTION_CLAUSE_NONE }, /* R2 sibling B (shared pad) */
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-sibling-group-multirange-one-per-range";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 3, base, code_len, out);
		bool good = ok && out.size () == 6 &&
			/* the four sibling base catches occupy the lower slots */
			out[0].clause_index == 0 && out[1].clause_index == 1 &&
			out[2].clause_index == 0 && out[3].clause_index == 1 &&
			out[0].try_start == (gpointer) (base + 0x10) &&
			out[2].try_start == (gpointer) (base + 0x28) &&
			/* exactly one synthesised finally PER distinct range */
			out[4].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[4].clause_index == 2 &&
			out[5].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[5].clause_index == 2 &&
			out[4].try_start == out[0].try_start && out[4].try_end == out[0].try_end &&
			out[4].handler_start == out[0].handler_start &&
			out[5].try_start == out[2].try_start && out[5].try_end == out[2].try_end &&
			out[5].handler_start == out[2].handler_start &&
			out[4].try_start != out[5].try_start;
		if (!good) {
			printf ("FAIL nest-sibling-group-multirange-one-per-range: ok=%d size=%zu "
			        "(expected 6: 4 base + one finally per distinct range)\n", ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-sibling-group-multirange-one-per-range (6 ei: finally synthesised once per distinct range)\n");
		}
	}

	/*
	 * (11) DEPTH-3 try/finally x3 - the enclosing-entry ORDER at depth >= 3 (doc 21 4.1,
	 * EH N6). clause0 (inner finally) is nested in clause1 (middle finally) is nested in
	 * clause2 (outer finally): nested_in[0] = {1, 2}. The single inner base entry
	 * synthesises TWO enclosing finally entries. They MUST be appended in ASCENDING
	 * clause_index (1 then 2) = innermost-encloser first: pass-2 resumes at the running
	 * clause's ARRAY slot + 1, so the array order [inner@0, middle@1, outer@2] is what
	 * makes the runtime run the finallys inner-to-outer (C, B, A). This is the exact
	 * property a DESCENDING order (legacy mini-llvm.c:3821 prepend, which would give
	 * [inner@0, outer@2, middle@1]) would get wrong. Assert the clause_index sequence is
	 * 0,1,2 and every entry shares the inner base's range + landing pad.
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

		std::vector<MonoLsdaEntry> ents = { { R_START, R_LEN, H_OFF, 0, MONO_EXCEPTION_CLAUSE_FINALLY } };
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-depth3-finally-ascending-order";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 3, base, code_len, out);
		bool good = ok && out.size () == 3 &&
			/* slot 0: base inner finally */
			out[0].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[0].clause_index == 0 &&
			out[0].try_start == (gpointer) (base + R_START) &&
			out[0].try_end == (gpointer) (base + R_START + R_LEN) &&
			out[0].handler_start == (gpointer) MINI_ADDR_TO_FTNPTR (base + H_OFF) &&
			/* slots 1,2: enclosing finallys, ASCENDING clause_index (middle before outer) */
			out[1].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[1].clause_index == 1 &&
			out[2].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[2].clause_index == 2 &&
			/* every synthesised encloser shares the inner base's range + landing pad */
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
	 * (12) DEPTH-4 try/finally x4 - the same ordering over THREE enclosers of one base.
	 * nested_in[0] = {1, 2, 3}. Assert FOUR ei with clause_index 0,1,2,3 (ascending =
	 * innermost-encloser first) all over the inner base's range/landing pad.
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

		std::vector<MonoLsdaEntry> ents = { { R_START, R_LEN, H_OFF, 0, MONO_EXCEPTION_CLAUSE_FINALLY } };
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
	 * (13) DEPTH-3 DE-DUP - a sibling catch group enclosed by TWO finallys. clause0/clause1
	 * are inner sibling catches (identical try range), enclosed by clause2 (middle finally)
	 * and clause3 (outer finally): nested_in[0] = nested_in[1] = {2, 3}. The gather hands
	 * TWO sibling base entries over the SAME range/landing pad. Each would re-synthesise the
	 * SAME two enclosing finallys; the (range, enclosing clause_index) de-dup collapses them
	 * so EACH encloser appears exactly ONCE. Expect FOUR ei: two sibling base catches
	 * (slots 0,1) then the two enclosing finallys ONCE each in ascending order (slots 2,3) -
	 * NOT six (un-deduped) - proving the dedup and the depth>=3 order compose.
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

		/* Two sibling base entries: SAME range, SAME (shared) landing pad 0x40. */
		std::vector<MonoLsdaEntry> ents = {
			{ 0x10, 0x10, 0x40, 0, MONO_EXCEPTION_CLAUSE_NONE },
			{ 0x10, 0x10, 0x40, 1, MONO_EXCEPTION_CLAUSE_NONE },
		};
		std::vector<MonoJitExceptionInfo> out;
		current_case = "nest-depth3-sibling-dedup-ascending";
		cases_run ++;
		bool ok = mono::build_ex_info (ents, cl, 4, base, code_len, out);
		bool good = ok && out.size () == 4 &&
			/* slots 0,1: the two sibling base catches over the shared range/pad */
			out[0].flags == MONO_EXCEPTION_CLAUSE_NONE && out[0].clause_index == 0 &&
			out[1].flags == MONO_EXCEPTION_CLAUSE_NONE && out[1].clause_index == 1 &&
			out[0].try_start == out[1].try_start && out[0].handler_start == out[1].handler_start &&
			/* slots 2,3: the two enclosing finallys, ONCE each, ascending clause_index */
			out[2].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[2].clause_index == 2 &&
			out[3].flags == MONO_EXCEPTION_CLAUSE_FINALLY && out[3].clause_index == 3 &&
			out[2].try_start == out[0].try_start && out[2].handler_start == out[0].handler_start &&
			out[3].try_start == out[0].try_start && out[3].handler_start == out[0].handler_start;
		if (!good) {
			printf ("FAIL nest-depth3-sibling-dedup-ascending: ok=%d size=%zu "
			        "(expected 4: 2 sibling catches + 2 enclosers once each; 6 == un-deduped)\n",
			        ok, out.size ());
			failures ++;
		} else {
			printf ("ok   nest-depth3-sibling-dedup-ascending (4 ei: 2 siblings, 2 enclosers deduped ascending)\n");
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
	 * (1) Happy path: two valid disjoint entries against a real cfg/header pair
	 * publish exactly what build_ex_info would produce for the same inputs
	 * (cross-checked against cases_mono_lsda_build's equivalent vectors).
	 */
	{
		MonoCompile cfg;
		memset (&cfg, 0, sizeof (cfg));
		cfg.mempool = pool;
		cfg.header = &header;

		std::vector<MonoLsdaEntry> ents = {
			{ 0x10, 0x20, 0x40, 0 }, /* [0x10,0x30) -> handler 0x40, clause 0 */
			{ 0x50, 0x10, 0x80, 1 }, /* [0x50,0x60) -> handler 0x80, clause 1 */
		};
		current_case = "publish-valid-two-clause";
		cases_run ++;
		bool ok = mono::publish_mono_lsda (&cfg, ents, base, code_len);
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
	 * (2) cfg->header == nullptr (no IL clause table at all) with no entries:
	 * num_clauses collapses to 0 and clauses to nullptr (the ternary at the top
	 * of publish_mono_lsda), which is the vacuous accept case - and since the
	 * built array is empty, the n==0 branch must skip the mempool allocation
	 * entirely, leaving llvm_ex_info null.
	 */
	{
		MonoCompile cfg;
		memset (&cfg, 0, sizeof (cfg));
		cfg.mempool = pool;
		cfg.header = nullptr;

		std::vector<MonoLsdaEntry> ents;
		current_case = "publish-no-header-empty-entries";
		cases_run ++;
		bool ok = mono::publish_mono_lsda (&cfg, ents, base, code_len);
		bool good = ok && cfg.llvm_ex_info == nullptr && cfg.llvm_ex_info_len == 0;
		if (!good) {
			printf ("FAIL publish-no-header-empty-entries: ok=%d ptr=%p len=%u\n",
			        ok, (void*) cfg.llvm_ex_info, cfg.llvm_ex_info_len);
			failures ++;
		} else {
			printf ("ok   publish-no-header-empty-entries (llvm_ex_info_len=0)\n");
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
