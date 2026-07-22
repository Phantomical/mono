/*
 * test-llvm-ehtable.cpp: unit tests for the stock-Itanium .gcc_except_table
 * (LSDA) decoder in mono/mini/llvm/lsda.cpp.
 *
 * decode_gcc_except_table() turns the .gcc_except_table LLVM 18 emits into a
 * ParsedLsda: the header, the call-site table, and per call site the action
 * chain resolved to ttype/clause indices. This is milestone M1 - an OFFLINE
 * decoder with no runtime wiring - so everything here drives it with byte
 * buffers directly, the same way test-llvm-ehframe.c drives the .eh_frame
 * transcoder.
 *
 * ---- the two known-good vectors ----
 *
 * VECTOR_CLANG18 is the PRIMARY, reproducible known-good vector: the exact
 * .gcc_except_table clang-18 emits for a two-catch C++ try/catch in mono's JIT
 * target configuration (static relocation model, small code model). It was
 * produced by, and is reproducible with:
 *
 *   cat > ex.cpp <<'EOF'
 *   struct A {}; struct B {};
 *   void may_throw(); void h_a(); void h_b();
 *   void f() {
 *     try { may_throw(); }
 *     catch (A&) { h_a(); }
 *     catch (B&) { h_b(); }
 *   }
 *   EOF
 *   /usr/lib/llvm-18/bin/clang++ -O1 -fexceptions \
 *       -fno-pic -fno-pie -mcmodel=small -static -c ex.cpp -o ex.o
 *   /usr/lib/llvm-18/bin/llvm-objdump -s -j .gcc_except_table ex.o
 *
 * Those flags reproduce the header the doc records in
 * 07-exception-handling.md 2.2: LPStart = omit (0xff), TType = udata4 (0x03,
 * absolute - llvm-readelf -r shows R_X86_64_32 ttype relocations, matching doc
 * 2.5), call-site = uleb128 (0x01). Clang's DEFAULT (PIC) build instead emits
 * TType = indirect|pcrel|sdata4 (0x9b), which the decoder declines (CAP-EH-0);
 * that is the discrepancy noted in the report and the reason for the static
 * flags.
 *
 * VECTOR_DOC is the vector quoted verbatim in doc 2.2 whose correct decoding the
 * doc states by hand. The doc's byte dump is truncated ("...") one byte before
 * its ttype table, so the last 8 bytes here are the 2-entry ttype table
 * reconstructed to reach the ttbase the doc's own header points at (offset
 * 0x20). Those 8 bytes are ttype ENTRIES - R_X86_64_32 relocations the loader
 * resolves - which this decoder never reads, so their value is immaterial; they
 * exist only so the buffer physically extends to ttbase. This vector cross-
 * checks the decoder against the doc's hand-decode: call site 1 -> TypeInfo 1,
 * call site 2 -> TypeInfo 2 (then cleanup), call site 3 -> no landing pad.
 *
 * ---- portability ----
 *
 * Unlike test-llvm-ehframe.c this file is TARGET-INDEPENDENT: the LSDA is a pure
 * byte format with no register numbers or target scaling, so the decoder and
 * these vectors mean the same thing everywhere. It runs its cases on every host.
 *
 * ---- the guard-page runner ----
 *
 * Every buffer is decoded with its last byte flush against a PROT_NONE guard
 * page (decode_guarded), so any read one byte past the declared size faults
 * immediately with a legible crash rather than reading adjacent memory and
 * returning a plausible-but-wrong answer. The truncation sweep - decoding every
 * prefix length of a valid table - leans on this: a decoder that forgets a
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

#include "mini/llvm/lsda.hpp"

using mono::ParsedLsda;
using mono::LsdaCallSite;

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

/*
 * Decode LEN bytes of DATA with the final byte flush against a PROT_NONE guard
 * page, so an over-read faults instead of silently succeeding. Falls back to a
 * plain heap buffer where mmap is unavailable.
 */
static bool
decode_guarded (const std::uint8_t *data, std::size_t len, ParsedLsda &out)
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
				bool r = mono::decode_gcc_except_table (buf, len, out);
				munmap (base, 2 * page);
				return r;
			}
			munmap (base, 2 * page);
		}
	}
#endif
	/* Fallback: exact-size heap buffer (still catches most over-reads under ASan). */
	std::vector<std::uint8_t> buf (data, data + len);
	return mono::decode_gcc_except_table (buf.empty () ? nullptr : buf.data (), len, out);
}

/* ------------------------------------------------------------ known-good */

/*
 * clang-18 -O1 -fexceptions -fno-pic -fno-pie -mcmodel=small -static, two-catch
 * try/catch (see the file header). 56 bytes; ttbase (offset 0x38) is the buffer
 * end. 8 call sites; call site 1's action chain is TypeInfo 2 -> TypeInfo 1,
 * call site 7's is TypeInfo 3; the rest are cleanup-only (no landing pad or
 * action 0). The trailing ttype table (offsets 0x2c..0x37) is R_X86_64_32
 * relocations, zero in the object and never read by the decoder.
 */
static const std::uint8_t VECTOR_CLANG18 [] = {
	0xff, 0x03, 0x35, 0x01, 0x20, 0x01, 0x05, 0x08,
	0x03, 0x06, 0x12, 0x00, 0x00, 0x18, 0x05, 0x41,
	0x00, 0x1d, 0x0f, 0x00, 0x00, 0x2c, 0x05, 0x37,
	0x00, 0x31, 0x09, 0x00, 0x00, 0x3a, 0x0f, 0x51,
	0x05, 0x49, 0x10, 0x00, 0x00, 0x01, 0x00, 0x02,
	0x7d, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/*
 * The exact bytes quoted in doc 2.2 (offsets 0x00..0x17), plus an 8-byte
 * 2-entry ttype table reconstructed to reach the doc's ttbase (offset 0x20);
 * those 8 bytes are relocations the decoder never reads. 32 bytes, 3 call sites.
 */
static const std::uint8_t VECTOR_DOC [] = {
	0xff, 0x03, 0x1d, 0x01, 0x0c, 0x01, 0x05, 0x22,
	0x05, 0x06, 0x05, 0x0d, 0x03, 0x0b, 0x23, 0x00,
	0x00, 0x00, 0x00, 0x02, 0x7d, 0x01, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Expected shape of one call site. actn < 0 in a slot is never expected. */
struct ExpCS {
	std::uint32_t start, length, landing_pad;
	int n_ttype;
	std::int32_t ttype [4];
};

static void
check_call_site (const char *what, int i, const LsdaCallSite &got, const ExpCS &exp)
{
	if (got.start != exp.start || got.length != exp.length ||
	    got.landing_pad != exp.landing_pad) {
		printf ("FAIL %s: cs %d fields: got {start=%u len=%u lp=%u}, "
		        "want {start=%u len=%u lp=%u}\n", what, i,
		        got.start, got.length, got.landing_pad,
		        exp.start, exp.length, exp.landing_pad);
		failures ++;
		return;
	}
	if ((int) got.ttype_indices.size () != exp.n_ttype) {
		printf ("FAIL %s: cs %d chain length: got %d, want %d\n", what, i,
		        (int) got.ttype_indices.size (), exp.n_ttype);
		failures ++;
		return;
	}
	for (int k = 0; k < exp.n_ttype; ++k) {
		if (got.ttype_indices [k] != exp.ttype [k]) {
			printf ("FAIL %s: cs %d ttype[%d]: got %d, want %d\n", what, i, k,
			        (int) got.ttype_indices [k], (int) exp.ttype [k]);
			failures ++;
			return;
		}
	}
}

static void
expect_decode (const char *what, const std::uint8_t *data, std::size_t len,
               std::uint8_t lp_enc, std::uint8_t tt_enc, std::uint8_t cs_enc,
               bool has_ttype, std::size_t ttbase_off, std::size_t ttype_count,
               const ExpCS *exp, int nexp)
{
	ParsedLsda out;

	current_case = what;
	cases_run ++;
	if (!decode_guarded (data, len, out)) {
		printf ("FAIL %s: declined a valid table\n", what);
		failures ++;
		return;
	}
	if (out.lpstart_encoding != lp_enc || out.ttype_encoding != tt_enc ||
	    out.call_site_encoding != cs_enc || out.has_ttype_table != has_ttype ||
	    out.ttype_base_offset != ttbase_off || out.ttype_entry_count != ttype_count) {
		printf ("FAIL %s: header: got {lp=%02x tt=%02x cs=%02x has=%d ttbase=%zu cnt=%zu}, "
		        "want {lp=%02x tt=%02x cs=%02x has=%d ttbase=%zu cnt=%zu}\n", what,
		        out.lpstart_encoding, out.ttype_encoding, out.call_site_encoding,
		        out.has_ttype_table, out.ttype_base_offset, out.ttype_entry_count,
		        lp_enc, tt_enc, cs_enc, has_ttype, ttbase_off, ttype_count);
		failures ++;
		return;
	}
	if ((int) out.call_sites.size () != nexp) {
		printf ("FAIL %s: call-site count: got %d, want %d\n", what,
		        (int) out.call_sites.size (), nexp);
		failures ++;
		return;
	}
	int before = failures;
	for (int i = 0; i < nexp; ++i)
		check_call_site (what, i, out.call_sites [i], exp [i]);
	if (failures == before)
		printf ("ok   %s (%d call sites)\n", what, nexp);
}

static void
expect_decline (const char *what, const std::uint8_t *data, std::size_t len)
{
	ParsedLsda out;

	current_case = what;
	cases_run ++;
	if (decode_guarded (data, len, out)) {
		printf ("FAIL %s: accepted a table that should decline\n", what);
		failures ++;
	} else {
		printf ("ok   %s (declined)\n", what);
	}
}

/* ------------------------------------------------------------ cases */

static void
cases_known_good (void)
{
	/* VECTOR_CLANG18: 8 call sites; only 1 and 7 carry catch actions. */
	static const ExpCS clang_exp [] = {
		{  1,  5,  8, 2, { 2, 1, 0, 0 } }, /* cs1: TypeInfo 2 -> TypeInfo 1 */
		{  6, 18,  0, 0, { 0, 0, 0, 0 } }, /* cs2: cleanup */
		{ 24,  5, 65, 0, { 0, 0, 0, 0 } }, /* cs3 */
		{ 29, 15,  0, 0, { 0, 0, 0, 0 } }, /* cs4 */
		{ 44,  5, 55, 0, { 0, 0, 0, 0 } }, /* cs5 */
		{ 49,  9,  0, 0, { 0, 0, 0, 0 } }, /* cs6 */
		{ 58, 15, 81, 1, { 3, 0, 0, 0 } }, /* cs7: TypeInfo 3 */
		{ 73, 16,  0, 0, { 0, 0, 0, 0 } }, /* cs8 */
	};
	expect_decode ("clang18-static-known-good", VECTOR_CLANG18, sizeof (VECTOR_CLANG18),
	               0xff, 0x03, 0x01, true, 0x38, 4, clang_exp, 8);

	/* VECTOR_DOC: the doc's hand-decoded 3-call-site table. */
	static const ExpCS doc_exp [] = {
		{  1,  5, 34, 1, { 1, 0, 0, 0 } }, /* cs1: TypeInfo 1 */
		{  6,  5, 13, 2, { 2, 0, 0, 0 } }, /* cs2: TypeInfo 2 -> cleanup (0) */
		{ 11, 35,  0, 0, { 0, 0, 0, 0 } }, /* cs3: no landing pad */
	};
	expect_decode ("doc-2.2-known-good", VECTOR_DOC, sizeof (VECTOR_DOC),
	               0xff, 0x03, 0x01, true, 0x20, 3, doc_exp, 3);
}

/*
 * Catch-by-type among siblings: distinct call sites resolving to distinct ttype
 * indices. Both vectors above already exercise this (clang cs1->2, cs7->3; doc
 * cs1->1, cs2->2). This builds a minimal hand-made table that isolates it: two
 * catching call sites, two ttype indices, one cleanup, asserting each maps to
 * its own index.
 */
static void
cases_siblings (void)
{
	/*
	 * header: lp=omit, tt=udata4, ttbase off, cs=uleb, cs table len.
	 * We compute ttbase to sit at the buffer end after a 2-entry ttype table.
	 */
	std::vector<std::uint8_t> b;
	/* Call-site table: 3 records x 4 bytes = 12 bytes.
	 *   cs0: start=0 len=4 lp=10 action=1  -> action rec @0: ttype 1
	 *   cs1: start=4 len=4 lp=20 action=3  -> action rec @2: ttype 2
	 *   cs2: start=8 len=4 lp=0  action=0  -> cleanup
	 */
	const std::uint8_t cs [] = {
		0, 4, 10, 1,
		4, 4, 20, 3,
		8, 4, 0,  0,
	};
	/* action table: two records, both terminal.
	 *   @0: ttype 1, disp 0
	 *   @2: ttype 2, disp 0
	 */
	const std::uint8_t at [] = { 1, 0, 2, 0 };
	/* ttype table: 2 entries x 4 bytes (values irrelevant, never read). */
	const std::uint8_t tt [8] = { 0 };

	/*
	 * Assemble the body (call-site enc byte onward). ttbase sits at the buffer
	 * END: the ttype table grows BACKWARD from it, so it comes after the action
	 * table, and ttbase_from_body is measured once the whole body is built.
	 */
	std::vector<std::uint8_t> body;
	body.push_back (0x01);                       /* call-site enc = uleb */
	body.push_back ((std::uint8_t) sizeof (cs)); /* cs table len */
	body.insert (body.end (), cs, cs + sizeof (cs));
	body.insert (body.end (), at, at + sizeof (at));
	body.insert (body.end (), tt, tt + sizeof (tt));
	std::size_t ttbase_from_body = body.size (); /* one past the last ttype entry */

	b.push_back (0xff); /* LPStart omit */
	b.push_back (0x03); /* TType udata4 */
	/* ttbase offset is measured from the byte after this uleb; body starts there. */
	b.push_back ((std::uint8_t) ttbase_from_body);
	b.insert (b.end (), body.begin (), body.end ());

	std::size_t ttbase_off = 3 + ttbase_from_body; /* == b.size (), the buffer end */
	/*
	 * ttype_entry_count is the UPPER BOUND: (ttbase - action_table)/4. The action
	 * table (4 bytes) sits between the call-site table end and the ttype table (8
	 * bytes), so (12 bytes)/4 = 3, though only 2 entries are real. Indices 1 and 2
	 * are both within it.
	 */
	static const ExpCS exp [] = {
		{ 0, 4, 10, 1, { 1, 0, 0, 0 } },
		{ 4, 4, 20, 1, { 2, 0, 0, 0 } },
		{ 8, 4,  0, 0, { 0, 0, 0, 0 } },
	};
	expect_decode ("siblings-two-types", b.data (), b.size (),
	               0xff, 0x03, 0x01, true, ttbase_off, 3, exp, 3);
}

/* A cleanup-only table with the TType encoding omitted (no catch clauses). */
static void
cases_ttype_omit (void)
{
	/* lp=omit, tt=omit (no ttbase field), cs=uleb, len=4, one cleanup record. */
	const std::uint8_t buf [] = {
		0xff, 0xff, 0x01, 0x04,
		0x00, 0x08, 0x00, 0x00, /* start=0 len=8 lp=0 action=0 */
	};
	static const ExpCS exp [] = {
		{ 0, 8, 0, 0, { 0, 0, 0, 0 } },
	};
	expect_decode ("ttype-omit-cleanup-only", buf, sizeof (buf),
	               0xff, 0xff, 0x01, false, 0, 0, exp, 1);
}

static void
cases_negative (void)
{
	/* Truncation sweep: every proper prefix of a valid table must decline and
	 * must not read past its (guarded) end. */
	{
		bool ok = true;
		for (std::size_t len = 0; len < sizeof (VECTOR_CLANG18); ++len) {
			ParsedLsda out;
			current_case = "truncation-sweep";
			if (decode_guarded (VECTOR_CLANG18, len, out)) {
				printf ("FAIL truncation-sweep: prefix len %zu was accepted\n", len);
				failures ++;
				ok = false;
				break;
			}
		}
		cases_run ++;
		if (ok)
			printf ("ok   truncation-sweep (%zu prefixes declined)\n",
			        sizeof (VECTOR_CLANG18) - 1);
	}

	/* Bogus header encodings. */
	{
		std::uint8_t b [sizeof (VECTOR_CLANG18)];
		memcpy (b, VECTOR_CLANG18, sizeof b);
		b [0] = 0x1b; /* LPStart pcrel|sdata4: not omit */
		expect_decline ("lpstart-encoding-not-omit-declines", b, sizeof b);
	}
	{
		std::uint8_t b [sizeof (VECTOR_CLANG18)];
		memcpy (b, VECTOR_CLANG18, sizeof b);
		b [1] = 0x9b; /* TType indirect|pcrel|sdata4: what a PIC build emits */
		expect_decline ("ttype-encoding-indirect-declines", b, sizeof b);
	}
	{
		std::uint8_t b [sizeof (VECTOR_CLANG18)];
		memcpy (b, VECTOR_CLANG18, sizeof b);
		b [1] = 0x00; /* TType absptr: unsupported */
		expect_decline ("ttype-encoding-absptr-declines", b, sizeof b);
	}
	{
		std::uint8_t b [sizeof (VECTOR_CLANG18)];
		memcpy (b, VECTOR_CLANG18, sizeof b);
		b [3] = 0x03; /* call-site encoding udata4: not uleb128 */
		expect_decline ("call-site-encoding-not-uleb-declines", b, sizeof b);
	}

	/* TType base offset that runs past the buffer end. */
	{
		std::uint8_t b [] = {
			0xff, 0x03, 0x7f, /* ttbase off = 0x7f, far past end */
			0x01, 0x00,       /* cs enc uleb, cs len 0 */
		};
		expect_decline ("ttype-base-offset-past-buffer-declines", b, sizeof b);
	}

	/* Call-site table length that runs past the buffer end. */
	{
		std::uint8_t b [sizeof (VECTOR_CLANG18)];
		memcpy (b, VECTOR_CLANG18, sizeof b);
		b [4] = 0x40; /* cs table len 64 > remaining */
		expect_decline ("call-site-table-length-past-buffer-declines", b, sizeof b);
	}

	/* A call-site record truncated mid-field (last record loses its action). */
	{
		/* Header + one 3-byte record where a 4th uleb field is missing. */
		std::uint8_t b [] = {
			0xff, 0xff, 0x01, 0x03, /* tt omit; cs len = 3 (too short for a record) */
			0x00, 0x08, 0x00,       /* start, length, landing_pad; action MISSING */
		};
		expect_decline ("call-site-record-truncated-declines", b, sizeof b);
	}

	/* Action field pointing past the action table. */
	{
		std::uint8_t b [] = {
			0xff, 0xff, 0x01, 0x04,
			0x00, 0x08, 0x00, 0x7f, /* action = 0x7f, way past a 0-byte action table */
		};
		expect_decline ("action-index-past-table-declines", b, sizeof b);
	}

	/* Action chain cycle: a record whose next_disp points back to itself. */
	{
		/*
		 * tt=udata4, ttbase after action table. One catch call site with action=1
		 * -> action rec @0: ttype 1, disp -2 (self-relative to the disp field at
		 * action offset 1 -> target 1 + (-2) = -1 ... use a 2-record loop instead).
		 * Simpler: rec @0 disp points to itself: disp field at offset 1, disp = -1
		 * -> target 0, an infinite self-loop the guard must break.
		 */
		std::uint8_t b [] = {
			0xff, 0x03, 0x0a, /* ttbase off 0x0a from body start */
			0x01, 0x04,       /* cs enc uleb, cs len 4 */
			0x00, 0x04, 0x0a, 0x01, /* start len lp action=1 */
			0x01, 0x7f,       /* action rec @0: ttype 1, disp sleb 0x7f = -1 -> self */
			0x00, 0x00, 0x00, 0x00, /* 1-entry ttype table */
		};
		expect_decline ("action-chain-cycle-declines", b, sizeof b);
	}

	/* ttype index out of range: a catch action naming an index beyond the table. */
	{
		std::uint8_t b [] = {
			0xff, 0x03, 0x08, /* ttbase off 8 from body start */
			0x01, 0x04,       /* cs enc uleb, cs len 4 */
			0x00, 0x04, 0x0a, 0x01, /* one catch call site, action=1 */
			0x09, 0x00,       /* action rec @0: ttype 9 (only room for ~1 entry), disp 0 */
			0x00, 0x00, 0x00, 0x00, /* 1-entry ttype table */
		};
		expect_decline ("ttype-index-out-of-range-declines", b, sizeof b);
	}

	/* A filter action (negative ttype index) - unsupported by M1's catch-only
	 * model. */
	{
		std::uint8_t b [] = {
			0xff, 0x03, 0x08,
			0x01, 0x04,
			0x00, 0x04, 0x0a, 0x01, /* action=1 */
			0x7f, 0x00,             /* action rec @0: ttype sleb 0x7f = -1 (filter) */
			0x00, 0x00, 0x00, 0x00,
		};
		expect_decline ("filter-action-declines", b, sizeof b);
	}

	/* Empty buffer and a null pointer must both decline without touching memory. */
	{
		ParsedLsda out;
		current_case = "empty-buffer-declines";
		cases_run ++;
		if (mono::decode_gcc_except_table (nullptr, 0, out)) {
			printf ("FAIL empty-buffer-declines: accepted null\n");
			failures ++;
		} else {
			const std::uint8_t nothing [1] = { 0 };
			if (decode_guarded (nothing, 0, out)) {
				printf ("FAIL empty-buffer-declines: accepted zero-length\n");
				failures ++;
			} else {
				printf ("ok   empty-buffer-declines (declined)\n");
			}
		}
	}
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

	cases_known_good ();
	cases_siblings ();
	cases_ttype_omit ();
	cases_negative ();

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
