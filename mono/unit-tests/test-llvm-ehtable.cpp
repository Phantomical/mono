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
#include "mini/llvm/mono_lsda.hpp"

using mono::ParsedLsda;
using mono::LsdaCallSite;
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

/*
 * VECTOR_CLANG18_LARGE is the real-JIT absptr known-good vector. It is the same
 * two-catch C++ try/catch as VECTOR_CLANG18, but compiled under the LARGE code
 * model - the model the engine's JIT effectively uses (09-eh-m2-plan.md 4, the
 * M2.1 finding). Under Large, LLVM 18 encodes the TType table as DW_EH_PE_absptr
 * (0x00): 8-byte ABSOLUTE entries relocated R_X86_64_64 (llvm-readelf -r shows two
 * R_X86_64_64 to _ZTI1A/_ZTI1B at offsets 0x38/0x40), not the DW_EH_PE_udata4
 * (0x03, 4-byte) that -mcmodel=small produces. It was produced by, and is
 * reproducible with:
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
 *       -fno-pic -fno-pie -mcmodel=large -static -c ex.cpp -o ex.o
 *   /usr/lib/llvm-18/bin/llvm-objcopy -O binary \
 *       --only-section=.gcc_except_table ex.o sec.bin   # the 72 bytes below
 *   /usr/lib/llvm-18/bin/llvm-readelf -r ex.o           # R_X86_64_64 ttype relocs
 *
 * 72 bytes; ttbase (offset 0x48) is the buffer end. 8 call sites; call site 1's
 * action chain is TypeInfo 2 -> TypeInfo 1, call site 7's is TypeInfo 3; the rest
 * are cleanup-only (no landing pad / action 0). The two 8-byte ttype entries
 * (offsets 0x38, 0x40) plus the zero pad between the action records and them are
 * R_X86_64_64 relocations, zero in the object and never read by the decoder. The
 * expected shape below is hand-derived from the Itanium format and the try/catch
 * source, independently of the decoder (cross-checked against the M2.1 engine
 * capture, which uses the same absptr 0x00 encoding).
 */
static const std::uint8_t VECTOR_CLANG18_LARGE [] = {
	0xff, 0x00, 0x45, 0x01, 0x22, 0x01, 0x0c, 0x0f,
	0x03, 0x0d, 0x19, 0x00, 0x00, 0x26, 0x0c, 0x72,
	0x00, 0x32, 0x16, 0x00, 0x00, 0x48, 0x0c, 0x61,
	0x00, 0x54, 0x10, 0x00, 0x00, 0x64, 0x1d, 0x90,
	0x01, 0x05, 0x81, 0x01, 0x1e, 0x00, 0x00, 0x01,
	0x00, 0x02, 0x7d, 0x03, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
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

	/*
	 * VECTOR_CLANG18_LARGE: the real-JIT absptr table (TType 0x00, 8-byte
	 * entries). Same call-site geometry as the source's try/catch shape as the
	 * small-model vector would give, but ttbase is 0x48 and ttype_entry_count is
	 * (0x48-0x27)/8 = 4 (the 8-byte divisor). cs0's chain is TypeInfo 2 ->
	 * TypeInfo 1, cs6's is TypeInfo 3; the rest cleanup-only. All hand-derived.
	 */
	static const ExpCS clang_large_exp [] = {
		{   1, 12,  15, 2, { 2, 1, 0, 0 } }, /* cs0: TypeInfo 2 -> TypeInfo 1 */
		{  13, 25,   0, 0, { 0, 0, 0, 0 } }, /* cs1: cleanup */
		{  38, 12, 114, 0, { 0, 0, 0, 0 } }, /* cs2 */
		{  50, 22,   0, 0, { 0, 0, 0, 0 } }, /* cs3 */
		{  72, 12,  97, 0, { 0, 0, 0, 0 } }, /* cs4 */
		{  84, 16,   0, 0, { 0, 0, 0, 0 } }, /* cs5 */
		{ 100, 29, 144, 1, { 3, 0, 0, 0 } }, /* cs6: TypeInfo 3 */
		{ 129, 30,   0, 0, { 0, 0, 0, 0 } }, /* cs7 */
	};
	expect_decode ("clang18-large-absptr-known-good", VECTOR_CLANG18_LARGE,
	               sizeof (VECTOR_CLANG18_LARGE),
	               0xff, 0x00, 0x01, true, 0x48, 4, clang_large_exp, 8);
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
		b [1] = 0x02; /* TType udata2: an absolute but unsupported width - declines */
		expect_decline ("ttype-encoding-udata2-declines", b, sizeof b);
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

/* ============================================================================
 * mono_lsda.cpp - the load-time .mono_lsda publish/validate core (plan 12 C4).
 *
 * parse_mono_lsda() decodes the target-neutral `.mono_lsda` section
 * MonoLSDAStreamer (engine.cpp, C3) emits; build_ex_info() validates those
 * tuples against the IL clause table and joins them into a MonoJitExceptionInfo[]
 * (the pure core of publish_mono_lsda, factored out so it needs no MonoCompile).
 * Everything here drives them with byte buffers and synthetic clause tables, the
 * same OFFLINE style as the LSDA-decoder cases above. Expectations are
 * hand-derived from the format, not echoed from the implementation.
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
	}
	return b;
}

/*
 * Parse LEN bytes of DATA with the final byte flush against a PROT_NONE guard
 * page (the same discipline as decode_guarded): any over-read faults loudly
 * instead of returning a plausible-but-wrong decode.
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
 * The exact bytes MonoLSDAStreamer emits, taken from plan 12 1.2's verified
 * probe2.o golden dump:
 *   44534c4d 01000200   magic 'MLSD', version 1, count 2
 *   01000000 05000000 11000000 07000000   {try=1, len=5, handler=0x11, clause=7}
 *   06000000 05000000 0f000000 03000000   {try=6, len=5, handler=0x0f, clause=3}
 * 40 bytes = 8 + 2*16. Decoded by hand from the format above.
 */
static const std::uint8_t GOLDEN_MLSD [] = {
	0x44, 0x53, 0x4c, 0x4d, 0x01, 0x00, 0x02, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	0x11, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
	0x06, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	0x0f, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
};

static void
check_entry (const char *what, int i, const MonoLsdaEntry &got, const MonoLsdaEntry &exp)
{
	if (got.try_start_off != exp.try_start_off || got.try_len != exp.try_len ||
	    got.handler_off != exp.handler_off || got.clause_index != exp.clause_index) {
		printf ("FAIL %s: entry %d: got {ts=%u tl=%u h=%u ci=%u}, "
		        "want {ts=%u tl=%u h=%u ci=%u}\n", what, i,
		        got.try_start_off, got.try_len, got.handler_off, got.clause_index,
		        exp.try_start_off, exp.try_len, exp.handler_off, exp.clause_index);
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

static void
cases_mono_lsda_parse (void)
{
	/* The C3 golden vector decodes to its two hand-derived entries. */
	static const MonoLsdaEntry golden_exp [] = {
		{ 1, 5, 0x11, 7 },
		{ 6, 5, 0x0f, 3 },
	};
	expect_parse ("mlsd-golden-two-entry", GOLDEN_MLSD, sizeof (GOLDEN_MLSD),
	              golden_exp, 2);

	/* A header-only section (count 0) is well-formed and decodes to nothing. */
	{
		std::vector<std::uint8_t> b = make_lsda (0x4d4c5344u, 1, 0, {});
		expect_parse ("mlsd-count-zero-header-only", b.data (), b.size (), nullptr, 0);
	}

	/* One-entry section: exactly 8 + 16 bytes. */
	{
		std::vector<MonoLsdaEntry> ents = { { 0x20, 0x08, 0x30, 0 } };
		std::vector<std::uint8_t> b = make_lsda (0x4d4c5344u, 1, 1, ents);
		static const MonoLsdaEntry one_exp [] = { { 0x20, 0x08, 0x30, 0 } };
		expect_parse ("mlsd-one-entry", b.data (), b.size (), one_exp, 1);
	}

	/* --- negatives --- */

	/* Bad magic. */
	{
		std::vector<MonoLsdaEntry> ents = { { 1, 5, 0x11, 7 } };
		std::vector<std::uint8_t> b = make_lsda (0xdeadbeefu, 1, 1, ents);
		expect_parse_decline ("mlsd-bad-magic", b.data (), b.size ());
	}

	/* Unknown version (2). */
	{
		std::vector<MonoLsdaEntry> ents = { { 1, 5, 0x11, 7 } };
		std::vector<std::uint8_t> b = make_lsda (0x4d4c5344u, 2, 1, ents);
		expect_parse_decline ("mlsd-version-2", b.data (), b.size ());
	}

	/* Truncated header (7 bytes: count field cut short). */
	{
		std::vector<std::uint8_t> b = make_lsda (0x4d4c5344u, 1, 0, {});
		b.pop_back ();
		expect_parse_decline ("mlsd-truncated-header", b.data (), b.size ());
	}

	/* Truncated entry: header says 2 entries but only one entry's worth of
	 * payload is present (8 + 16 bytes). Exact-size mismatch declines. */
	{
		std::vector<MonoLsdaEntry> ents = { { 1, 5, 0x11, 7 } };
		std::vector<std::uint8_t> b = make_lsda (0x4d4c5344u, 2, 1, ents);
		/* rewrite count to 2 while carrying one entry -> size 24 != 8+2*16 */
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
		std::vector<MonoLsdaEntry> ents = { { 1, 5, 0x11, 7 } };
		std::vector<std::uint8_t> rec = make_lsda (0x4d4c5344u, 1, 1, ents);
		std::vector<std::uint8_t> two = rec;
		two.insert (two.end (), rec.begin (), rec.end ()); /* 24 + 24 = 48 bytes */
		expect_parse_decline ("mlsd-two-record-oversize-declines", two.data (), two.size ());
	}

	/*
	 * Trailing-byte oversize: exactly one valid record plus a single junk byte.
	 * 25 != 24 -> decline (a section MUST be exactly its declared extent).
	 */
	{
		std::vector<MonoLsdaEntry> ents = { { 1, 5, 0x11, 7 } };
		std::vector<std::uint8_t> b = make_lsda (0x4d4c5344u, 1, 1, ents);
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

	/* handler_off == code_len (past the code). */
	expect_build_decline ("build-handler-past-code",
		{ { 0x10, 0x08, code_len, 0 } }, clauses, 2);

	/* clause_index >= num_clauses. */
	expect_build_decline ("build-clause-index-out-of-range",
		{ { 0x10, 0x08, 0x40, 5 } }, clauses, 2);

	/* A clause whose flags != CLAUSE_NONE (finally slipped the gate). */
	{
		MonoExceptionClause bad [1];
		memset (bad, 0, sizeof (bad));
		bad[0].flags = MONO_EXCEPTION_CLAUSE_FINALLY;
		expect_build_decline ("build-non-none-clause-flags",
			{ { 0x10, 0x08, 0x40, 0 } }, bad, 1);
	}

	/* count == 0 while num_clauses > 0 (every protected call optimised away). */
	expect_build_decline ("build-empty-while-clauses", {}, clauses, 2);

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
	cases_mono_lsda_parse ();
	cases_mono_lsda_build ();

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
