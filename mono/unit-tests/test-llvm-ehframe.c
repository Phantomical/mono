/*
 * test-llvm-ehframe.c: unit tests for the stock-DWARF .eh_frame transcoder.
 *
 * mono_llvm_eh_frame_to_unwind_ops() turns the .eh_frame LLVM emits into mono's
 * unwind ops. Roughly half of its opcode handling cannot be reached by running
 * managed code on x86-64: LLVM 18 unwinds epilogues with plain
 * DW_CFA_def_cfa_offset steps, so DW_CFA_restore, DW_CFA_restore_extended,
 * DW_CFA_undefined and the remember_state/restore_state pair never appear in
 * our own JIT output - yet all of them occur in real compiler output generally,
 * and any of them mishandled is a bad unwind during exception dispatch.
 *
 * So the CFI programs here are synthesized byte by byte, which also lets the
 * malformed cases be tested: those must DECLINE (return FALSE), never read out
 * of bounds.
 *
 * ---- how this file is organised ----
 *
 * Everything below is a NAMED case: one hand-built CIE/FDE byte sequence, one
 * behaviour, one assertion, one line of output. There are two shapes:
 *
 *   ok_case()      - the transcoder must accept, and the op list must be
 *                    EXACTLY the expected one (op, register, value and code
 *                    offset of every op, and the count).
 *   decline_case() - the transcoder must return FALSE and publish no ops.
 *
 * plus *_cie() variants that vary the CIE rather than the FDE program, and
 * accept_n_case() for the two boundary cases where only the op COUNT is
 * interesting.
 *
 * The generator at the end (test_random_programs) is deliberately NOT a golden
 * -bytes test: it fixes a seed, bounds its iteration count, and asserts
 * INVARIANTS of the output. Its predecessor - a differential harness against
 * the pre-C++ implementation - is not reproduced here, because it depended on
 * an implementation that no longer exists; what it measured that still matters
 * (the arithmetic thresholds) is pinned by the named cases instead.
 *
 * ---- THIS FILE IS x86-64 ONLY, AND SKIPS SILENTLY ELSEWHERE ----
 *
 * Every byte sequence here encodes x86-64 DWARF register numbers and offsets
 * factored by -8. test_llvm_ehframe_main() therefore returns 0 - a PASS -
 * without running a single case unless data_align == -8 and the return column
 * is 16. A green "test-llvm-ehframe" on any other target means NOTHING WAS
 * TESTED; do not read it as coverage. Making these cases portable would mean
 * deriving the register numbers from mono at runtime, which would test the
 * builder rather than the transcoder.
 *
 * ---- what these cases are calibrated against ----
 *
 * The exhaustive differential run behind commit d6915f5a770 measured, over
 * 17,784 real transcodes plus a randomised corpus:
 *
 *   - scale_by_data_align() range-checks |factored| > 2^31. That is the check's
 *     threshold, NOT the threshold at which the new code's VERDICT differs from
 *     the old unchecked multiply: between 2^31 and a full 2^64 wrap both
 *     decline, because the old (already overflowed) product was still outside
 *     every caller's 32-bit window. A verdict only flips once the wrapped
 *     product lands back inside an accepted window, which at data_align = -8
 *     first happens at |factored| = (2^64 - 2^31)/8 = 2^61 - 2^28 =
 *     2,305,843,008,945,258,496, whose product wraps to exactly -2^31, the
 *     bottom of the offset forms' window. Every flip is ACCEPT -> DECLINE;
 *     there is no input the new code accepts that the old one declined.
 *
 *     (An earlier revision of this comment, and of the project notes it came
 *     from, gave that boundary as 2,305,843,009,213,693,949. That number is
 *     2^61 - 3, not 2^61 - 2^28 - they differ by 268,435,453. It is a diverging
 *     operand, wrapping to -24, but it is not the smallest one. Both are pinned
 *     below, the boundary as the boundary and the other as what it is.)
 *   - advance() contributes no divergence at all and needs no threshold: it is
 *     computed unsigned and the loc_ > code_len check catches the wrap.
 *   - MAX_REMEMBER_DEPTH = 128 exactly: depth 128 is accepted, 129 declined.
 *   - The largest DW_CFA_offset factored value ever observed in real output is
 *     7, and only 1..7 occur, because x86-64 emits DW_CFA_offset only for
 *     callee-saved registers - so it is bounded by the callee-saved count and
 *     not by the frame size. The per-register cases below use exactly 1..7.
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <glib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#include "mini/mini.h"
#include "mini/mini-unwind.h"

#ifdef ENABLE_LLVM

/*
 * Declared here rather than by including llvm/backend.h: that header resolves
 * "mini.h" relative to its own directory, which does not work from here.
 *
 * (test-llvm-engine.cpp does NOT do this - it is a C++ TU and includes
 * mono/mini/llvm/engine.hpp directly. This file stays C because the transcoder
 * it exercises is reachable through the plain extern "C" boundary and needs no
 * LLVM headers at all.)
 */
gboolean mono_llvm_eh_frame_to_unwind_ops (guint8 *eh_frame, guint32 eh_frame_size,
					   gpointer code_start, guint32 code_len,
					   GSList **out_ops);

/* DW_EH_PE encodings. Spelled out here rather than pulled from a DWARF header
 * so the malformed ones can be named too. */
#define PE_absptr        0x00
#define PE_uleb128       0x01
#define PE_udata2        0x02
#define PE_udata4        0x03
#define PE_udata8        0x04
#define PE_sdata2        0x0a
#define PE_sdata4        0x0b
#define PE_sdata8        0x0c
#define PE_pcrel         0x10
#define PE_datarel       0x30
#define PE_indirect      0x80

#ifndef DW_CFA_GNU_args_size
#define DW_CFA_GNU_args_size 0x2e
#endif

/* MAX_UNWIND_OPS and MAX_REMEMBER_DEPTH from llvm/ehframe.cpp. Duplicated
 * deliberately: a change there must break a test here. */
#define EH_MAX_UNWIND_OPS     128
#define EH_MAX_REMEMBER_DEPTH 128

#define DATA_ALIGN (mono_unwind_get_dwarf_data_align ())
#define PC_REG     (mono_unwind_get_dwarf_pc_reg ())

/*
 * (2^64 - 2^31)/8 = 2^61 - 2^28 = 2,305,843,008,945,258,496: the smallest
 * |factored| whose product with data_align = -8 wraps a 64-bit multiply back
 * inside an accepted window (to exactly -2^31). See the file header.
 */
#define DIVERGENCE_BOUNDARY 2305843008945258496LL

/* Fixed generator seed: this test is reproducible, and the crash handler
 * prints it alongside the iteration number. */
#define FUZZ_SEED 0x1d0f2a3bu

/* ------------------------------------------------------------ builder */

typedef struct {
	guint8 buf [4096];
	int len;
	/* Offset of the FDE's initial_location field, for deriving code_start. */
	int pcbegin_off;
	/* Offset of the CIE the next FDE points back to. */
	int cie_off;
	/* Pointer encoding the CIE declared for FDE pointers ('R'). */
	guint8 fde_ptr_enc;
	/* Whether the CIE has a 'z' augmentation (so FDEs carry a length field). */
	gboolean cie_has_z;
	/* The code address the built FDE describes. */
	gpointer code_start;
} EhFrame;

static void
b_u8 (EhFrame *e, guint8 v)
{
	g_assert (e->len < (int)sizeof (e->buf));
	e->buf [e->len ++] = v;
}

static void
b_u32 (EhFrame *e, guint32 v)
{
	g_assert (e->len + 4 <= (int)sizeof (e->buf));
	memcpy (e->buf + e->len, &v, 4);
	e->len += 4;
}

static void
b_u64 (EhFrame *e, guint64 v)
{
	g_assert (e->len + 8 <= (int)sizeof (e->buf));
	memcpy (e->buf + e->len, &v, 8);
	e->len += 8;
}

static void
b_uleb (EhFrame *e, guint64 v)
{
	do {
		guint8 c = v & 0x7f;
		v >>= 7;
		if (v)
			c |= 0x80;
		b_u8 (e, c);
	} while (v);
}

static void
b_sleb (EhFrame *e, gint64 v)
{
	gboolean more = TRUE;
	while (more) {
		guint8 c = v & 0x7f;
		v >>= 7;
		if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40)))
			more = FALSE;
		else
			c |= 0x80;
		b_u8 (e, c);
	}
}

/*
 * A CFI program under construction. Same encoders as the section builder, so a
 * case never has to hand-encode a LEB128 - the ones that test LEB128 encodings
 * specifically emit their bytes literally with i_u8().
 */
typedef struct {
	guint8 b [1024];
	int n;
} Insn;

static void
i_u8 (Insn *p, guint8 v)
{
	g_assert (p->n < (int)sizeof (p->b));
	p->b [p->n ++] = v;
}

static void
i_uleb (Insn *p, guint64 v)
{
	do {
		guint8 c = v & 0x7f;
		v >>= 7;
		if (v)
			c |= 0x80;
		i_u8 (p, c);
	} while (v);
}

static void
i_sleb (Insn *p, gint64 v)
{
	gboolean more = TRUE;
	while (more) {
		guint8 c = v & 0x7f;
		v >>= 7;
		if ((v == 0 && !(c & 0x40)) || (v == -1 && (c & 0x40)))
			more = FALSE;
		else
			c |= 0x80;
		i_u8 (p, c);
	}
}

/* How many bytes a value with encoding ENC occupies. */
static int
enc_size (guint8 enc)
{
	switch (enc & 0x0f) {
	case PE_absptr: return (int) sizeof (gpointer);
	case PE_udata4:
	case PE_sdata4: return 4;
	case PE_udata8:
	case PE_sdata8: return 8;
	case PE_udata2:
	case PE_sdata2: return 2;
	default: return 4; /* unsupported: filler, the case declines anyway */
	}
}

/*
 * Everything about a CIE that a case might want to vary. cie_spec_init() fills
 * in exactly what LLVM emits on x86-64; a case overrides one field and names
 * itself for that field.
 */
typedef struct {
	int version;            /* 1 or 3 */
	const char *aug;        /* augmentation string, e.g. "zR" */
	guint64 code_align;
	gint64 data_align;
	guint64 return_reg;
	guint8 fde_ptr_enc;     /* the 'R' encoding */
	guint8 personality_enc; /* the 'P' encoding */
	guint8 lsda_enc;        /* the 'L' encoding */
	int aug_len_delta;      /* added to the declared augmentation length */
} CieSpec;

static void
cie_spec_init (CieSpec *s)
{
	memset (s, 0, sizeof (*s));
	s->version = 1;
	s->aug = "zR";
	s->code_align = 1;
	s->data_align = DATA_ALIGN;
	s->return_reg = (guint64) PC_REG;
	s->fde_ptr_enc = PE_pcrel | PE_sdata4;
	s->personality_enc = PE_pcrel | PE_sdata4;
	s->lsda_enc = PE_pcrel | PE_sdata4;
}

static void
build_cie_spec (EhFrame *e, const CieSpec *s, const guint8 *cie_insns, int cie_len)
{
	guint8 aug_data [64];
	int adn = 0;
	int len_off, start;
	const char *c;

	e->cie_off = e->len;
	len_off = e->len;
	b_u32 (e, 0);           /* length, patched below */
	start = e->len;
	b_u32 (e, 0);           /* CIE id */
	b_u8 (e, (guint8) s->version);
	for (c = s->aug; *c; ++c)
		b_u8 (e, (guint8) *c);
	b_u8 (e, 0);
	b_uleb (e, s->code_align);
	b_sleb (e, s->data_align);
	if (s->version == 1)
		b_u8 (e, (guint8) s->return_reg);
	else
		b_uleb (e, s->return_reg);

	e->cie_has_z = (s->aug [0] == 'z');
	e->fde_ptr_enc = PE_absptr;  /* what the parser defaults to without 'R' */

	if (e->cie_has_z) {
		int i;

		for (c = s->aug + 1; *c; ++c) {
			switch (*c) {
			case 'R':
				aug_data [adn ++] = s->fde_ptr_enc;
				e->fde_ptr_enc = s->fde_ptr_enc;
				break;
			case 'P':
				aug_data [adn ++] = s->personality_enc;
				for (i = 0; i < enc_size (s->personality_enc); ++i)
					aug_data [adn ++] = 0;
				break;
			case 'L':
				aug_data [adn ++] = s->lsda_enc;
				break;
			default:
				/* 'S' and anything unrecognised carry no data. */
				break;
			}
		}
		b_uleb (e, (guint64)(adn + s->aug_len_delta));
		for (i = 0; i < adn; ++i)
			b_u8 (e, aug_data [i]);
	}

	if (cie_len)
		memcpy (e->buf + e->len, cie_insns, cie_len), e->len += cie_len;
	while ((e->len - start) % 4)
		b_u8 (e, DW_CFA_nop);
	memcpy (e->buf + len_off, &(guint32){ e->len - start }, 4);
}

/*
 * Emit a CIE with augmentation "zR" (pcrel|sdata4 FDE pointers), code_align 1,
 * mono's data alignment, and mono's return column - i.e. exactly the shape LLVM
 * produces on this target. CIE_INSNS/CIE_LEN are the initial instructions.
 */
static void
build_cie (EhFrame *e, const guint8 *cie_insns, int cie_len)
{
	CieSpec s;

	cie_spec_init (&s);
	build_cie_spec (e, &s, cie_insns, cie_len);
}

/*
 * Emit an FDE for the CIE most recently built, with the given CFI program.
 * PAD adds the usual 4-byte alignment nops; a case that wants an operand
 * truncated by the end of the entry passes FALSE, or the padding would stand in
 * for the missing bytes.
 */
static void
build_fde_ex (EhFrame *e, guint32 code_len, const guint8 *insns, int insns_len, gboolean pad)
{
	int len_off, start, size;
	guint8 enc = e->fde_ptr_enc;

	len_off = e->len;
	b_u32 (e, 0);           /* length, patched below */
	start = e->len;
	b_u32 (e, (guint32)(e->len - e->cie_off)); /* distance back to the CIE */
	e->pcbegin_off = e->len;
	size = enc_size (enc);

	if ((enc & 0x70) == PE_pcrel) {
		/* initial_location, pc-relative; code_start derived from it */
		if (size == 4)
			b_u32 (e, 0x100);
		else
			b_u64 (e, 0x100);
		e->code_start = e->buf + e->pcbegin_off + 0x100;
	} else {
		/* Absolute (or an encoding the transcoder must refuse). */
		e->code_start = e->buf + 0x100;
		if (size == 4)
			b_u32 (e, (guint32)(gsize) e->code_start);
		else
			b_u64 (e, (guint64)(gsize) e->code_start);
	}

	if (size == 4)
		b_u32 (e, code_len);    /* address_range */
	else
		b_u64 (e, code_len);

	if (e->cie_has_z)
		b_uleb (e, 0);          /* augmentation length */
	if (insns_len)
		memcpy (e->buf + e->len, insns, insns_len), e->len += insns_len;
	while (pad && (e->len - start) % 4)
		b_u8 (e, DW_CFA_nop);
	memcpy (e->buf + len_off, &(guint32){ e->len - start }, 4);
}

static void
build_fde (EhFrame *e, guint32 code_len, const guint8 *insns, int insns_len)
{
	build_fde_ex (e, code_len, insns, insns_len, TRUE);
}

/* The code address the built FDE describes. */
static gpointer
code_start_of (EhFrame *e)
{
	return e->code_start;
}

/* ------------------------------------------------- crash legibility */

/*
 * What is running right now, so that a fatal signal can say so.
 *
 * The generator runs every section against a PROT_NONE guard page, so the
 * expected way for this file to die is SIGSEGV: the transcoder read past the end
 * of a section. That is a real finding and it has to arrive legible. Without
 * this it arrives as a bare "Segmentation fault" against an empty log, because
 * `make check` redirects stdout, stdout is then block buffered, and the buffer
 * dies with the process - the very outcome this corpus exists to prevent.
 *
 * Two halves: stdout is switched to line buffering in main() so everything
 * already printed is in the log, and the handler below names the case that was
 * in flight and has therefore printed nothing yet.
 */
static const char * volatile current_case = "(startup)";
static volatile sig_atomic_t current_iteration = -1;

#if defined (HAVE_UNISTD_H) && !defined (HOST_WIN32)

/* Async-signal-safe output: write(2) only. No printf, no allocation. */
static void
sig_write (const char *s)
{
	ssize_t ignored = write (2, s, strlen (s));

	(void) ignored;
}

static void
sig_write_int (int v)
{
	char buf [24];
	int n = 0;

	if (v < 0) {
		sig_write ("-");
		v = -v;
	}
	do {
		buf [n ++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v != 0 && n < (int) sizeof (buf));
	while (n --) {
		ssize_t ignored = write (2, buf + n, 1);
		(void) ignored;
	}
}

static void
crash_handler (int sig)
{
	sig_write ("\n*** test-llvm-ehframe died on signal ");
	sig_write_int (sig);
	sig_write (" in case: ");
	sig_write ((const char*) current_case);
	if (current_iteration >= 0) {
		sig_write ("\n*** generator iteration ");
		sig_write_int ((int) current_iteration);
		sig_write (" of the fixed seed ");
		sig_write_int ((int) FUZZ_SEED);
		sig_write (" - reproducible by rerunning this binary");
		sig_write ("\n*** SIGSEGV here means the GUARD PAGE fired: the transcoder read past"
			   "\n*** the end of the .eh_frame section it was given.");
	}
	sig_write ("\n");

	/* Re-raise with the default handler so the exit status and any core dump
	 * are what they would have been. */
	signal (sig, SIG_DFL);
	raise (sig);
}

static void
install_crash_handler (void)
{
	signal (SIGSEGV, crash_handler);
	signal (SIGBUS, crash_handler);
}

static void
remove_crash_handler (void)
{
	signal (SIGSEGV, SIG_DFL);
	signal (SIGBUS, SIG_DFL);
}

#else

static void install_crash_handler (void) { }
static void remove_crash_handler (void) { }

#endif

/* ------------------------------------------------------------ checking */

static int failures;
static int cases_run;

static const char *
op_name (int op)
{
	switch (op) {
	case DW_CFA_def_cfa: return "def_cfa";
	case DW_CFA_def_cfa_register: return "def_cfa_register";
	case DW_CFA_def_cfa_offset: return "def_cfa_offset";
	case DW_CFA_offset: return "offset";
	case DW_CFA_same_value: return "same_value";
	default: return "?";
	}
}

/* Expected op, in DWARF register numbering. dwarf_reg < 0 means "do not care". */
typedef struct { guint32 when; int op; int dwarf_reg; int val; } ExpOp;

static void
check_ops (const char *what, GSList *ops, const ExpOp *exp, int nexp)
{
	GSList *l = ops;
	int i = 0;

	cases_run ++;
	for (; l && i < nexp; l = l->next, ++i) {
		MonoUnwindOp *o = (MonoUnwindOp*) l->data;
		int got_reg = mono_hw_reg_to_dwarf_reg (o->reg);
		int want_reg = exp [i].dwarf_reg;

		if (o->when != exp [i].when || o->op != exp [i].op || o->val != exp [i].val ||
		    (want_reg >= 0 && got_reg != want_reg)) {
			printf ("FAIL %s: op %d: got {when=%u %s r%d val=%d}, want {when=%u %s r%d val=%d}\n",
				what, i, o->when, op_name (o->op), got_reg, o->val,
				exp [i].when, op_name (exp [i].op), want_reg, exp [i].val);
			failures ++;
			return;
		}
	}
	if (l || i != nexp) {
		printf ("FAIL %s: op count mismatch (extra=%s, matched=%d, want=%d)\n",
			what, l ? "yes" : "no", i, nexp);
		failures ++;
		return;
	}
	printf ("ok   %s (%d ops)\n", what, nexp);
}

/* Run the transcoder over a built section; returns TRUE on success. */
static gboolean
transcode (EhFrame *e, guint32 code_len, GSList **ops)
{
	return mono_llvm_eh_frame_to_unwind_ops (e->buf, (guint32) e->len,
						 code_start_of (e), code_len, ops);
}

static void
expect_decline (const char *what, EhFrame *e, guint32 code_len)
{
	GSList *ops = NULL;

	current_case = what;
	cases_run ++;
	if (transcode (e, code_len, &ops)) {
		printf ("FAIL %s: expected decline, got success\n", what);
		failures ++;
		mono_free_unwind_info (ops);
	} else if (ops) {
		printf ("FAIL %s: declined but returned ops\n", what);
		failures ++;
	} else {
		printf ("ok   %s (declined)\n", what);
	}
}

static void
expect_ops (const char *what, EhFrame *e, guint32 code_len, const ExpOp *exp, int nexp)
{
	GSList *ops = NULL;

	current_case = what;
	if (!transcode (e, code_len, &ops)) {
		printf ("FAIL %s: declined\n", what);
		failures ++;
		cases_run ++;
		return;
	}
	check_ops (what, ops, exp, nexp);
	mono_free_unwind_info (ops);
}

/* ------------------------------------------------------------ the cases */

/*
 * The CIE used throughout: CFA = r7 + 8, return column saved at cfa-8. That is
 * the standard x86-64 entry state and is what LLVM emits.
 */
static void
std_cie_insns (Insn *p)
{
	p->n = 0;
	i_u8 (p, DW_CFA_def_cfa); i_uleb (p, 7); i_uleb (p, 8);
	/* DW_CFA_offset(pc) with factored offset 1 => 1 * -8 = cfa-8 */
	i_u8 (p, (guint8)(DW_CFA_offset | PC_REG)); i_uleb (p, 1);
}

static void
std_cie (EhFrame *e)
{
	Insn p;

	std_cie_insns (&p);
	build_cie (e, p.b, p.n);
}

/*
 * The two ops every accepted FDE starts with: the CIE's rules restated at
 * offset 0, so the descriptor stands alone.
 */
#define PROLOGUE_OPS \
	{ 0, DW_CFA_def_cfa, 7,      8 }, \
	{ 0, DW_CFA_offset,  PC_REG, -8 }

/* Standard CIE + FDE running PROG; must accept, with exactly EXP. */
static void
ok_case (const char *name, const Insn *prog, guint32 code_len,
         const ExpOp *exp, int nexp)
{
	EhFrame e;

	memset (&e, 0, sizeof (e));
	std_cie (&e);
	build_fde (&e, code_len, prog->b, prog->n);
	expect_ops (name, &e, code_len, exp, nexp);
}

/* Standard CIE + FDE running PROG; must decline. */
static void
decline_case (const char *name, const Insn *prog, guint32 code_len)
{
	EhFrame e;

	memset (&e, 0, sizeof (e));
	std_cie (&e);
	build_fde (&e, code_len, prog->b, prog->n);
	expect_decline (name, &e, code_len);
}

/* Standard CIE + FDE running PROG; must accept with exactly N ops. */
static void
accept_n_case (const char *name, const Insn *prog, guint32 code_len, int n)
{
	EhFrame e;
	GSList *ops = NULL;
	int got = 0;
	GSList *l;

	memset (&e, 0, sizeof (e));
	std_cie (&e);
	build_fde (&e, code_len, prog->b, prog->n);

	current_case = name;
	cases_run ++;
	if (!transcode (&e, code_len, &ops)) {
		printf ("FAIL %s: declined\n", name);
		failures ++;
		return;
	}
	for (l = ops; l; l = l->next)
		got ++;
	if (got != n) {
		printf ("FAIL %s: got %d ops, want %d\n", name, got, n);
		failures ++;
	} else {
		printf ("ok   %s (%d ops)\n", name, got);
	}
	mono_free_unwind_info (ops);
}

/* A CIE built from SPEC (with CIE_PROG as its initial instructions) + an FDE
 * running PROG. */
static void
ok_case_cie (const char *name, const CieSpec *spec, const Insn *cie_prog,
             const Insn *prog, guint32 code_len, const ExpOp *exp, int nexp)
{
	EhFrame e;

	memset (&e, 0, sizeof (e));
	build_cie_spec (&e, spec, cie_prog->b, cie_prog->n);
	build_fde (&e, code_len, prog->b, prog->n);
	expect_ops (name, &e, code_len, exp, nexp);
}

static void
decline_case_cie (const char *name, const CieSpec *spec, const Insn *cie_prog,
                  const Insn *prog, guint32 code_len)
{
	EhFrame e;

	memset (&e, 0, sizeof (e));
	build_cie_spec (&e, spec, cie_prog->b, cie_prog->n);
	build_fde (&e, code_len, prog->b, prog->n);
	expect_decline (name, &e, code_len);
}

/* --------------------------------------------- DW_CFA_offset, per register */

/*
 * One case per callee-saved register on x86-64, plus the return column. The
 * factored values are 1..7 - the entire range ever observed in real output,
 * because DW_CFA_offset is emitted only for callee-saved registers.
 */
static void
cases_callee_saved_offsets (void)
{
	static const struct { const char *name; int reg; int factored; } tab [] = {
		{ "offset-rbx",           3,  1 },
		{ "offset-rbp",           6,  2 },
		{ "offset-r12",          12,  3 },
		{ "offset-r13",          13,  4 },
		{ "offset-r14",          14,  5 },
		{ "offset-r15",          15,  6 },
	};
	int i;

	for (i = 0; i < (int) G_N_ELEMENTS (tab); ++i) {
		Insn p;
		ExpOp exp [3];

		p.n = 0;
		i_u8 (&p, (guint8)(DW_CFA_offset | tab [i].reg));
		i_uleb (&p, (guint64) tab [i].factored);

		exp [0] = (ExpOp){ 0, DW_CFA_def_cfa, 7, 8 };
		exp [1] = (ExpOp){ 0, DW_CFA_offset, PC_REG, -8 };
		exp [2] = (ExpOp){ 0, DW_CFA_offset, tab [i].reg,
		                   tab [i].factored * DATA_ALIGN };
		ok_case (tab [i].name, &p, 16, exp, 3);
	}

	/* The return column: factored 7, the largest value ever observed. */
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_offset, PC_REG, 7 * -8 } };

		p.n = 0;
		i_u8 (&p, (guint8)(DW_CFA_offset | PC_REG)); i_uleb (&p, 7);
		ok_case ("offset-return-column", &p, 16, exp, G_N_ELEMENTS (exp));
	}

	/* Factored zero is representable: the register lives AT the CFA. */
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_offset, 3, 0 } };

		p.n = 0;
		i_u8 (&p, (guint8)(DW_CFA_offset | 3)); i_uleb (&p, 0);
		ok_case ("offset-factored-zero", &p, 16, exp, G_N_ELEMENTS (exp));
	}

	/* The primary opcode's 6-bit register field can name a register mono does
	 * not map. That must decline, not be silently dropped. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, (guint8)(DW_CFA_offset | 63)); i_uleb (&p, 1);
		decline_case ("offset-register-63-declines", &p, 16);
	}

	/* DW_CFA_offset_extended / _sf reach the same rule the long way. */
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_offset, 12, -24 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_offset_extended); i_uleb (&p, 12); i_uleb (&p, 3);
		ok_case ("offset-extended", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_offset, 12, -24 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_offset_extended_sf); i_uleb (&p, 12); i_sleb (&p, 3);
		ok_case ("offset-extended-sf", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/*
	 * A register saved ABOVE the CFA. mono encodes the factored value with
	 * encode_uleb128(), which takes a guint32, so this would come back out as a
	 * huge unsigned offset and be dereferenced during GC stack scanning.
	 */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_offset_extended_sf); i_uleb (&p, 12); i_sleb (&p, -1);
		decline_case ("offset-above-cfa-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_offset_extended); i_uleb (&p, 60); i_uleb (&p, 2);
		decline_case ("offset-extended-register-out-of-range-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_same_value); i_uleb (&p, 60);
		decline_case ("same-value-register-out-of-range-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_undefined); i_uleb (&p, 60);
		decline_case ("undefined-register-out-of-range-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_restore_extended); i_uleb (&p, 60);
		decline_case ("restore-extended-register-out-of-range-declines", &p, 16);
	}
}

/* ------------------------------------------- the scale_by_data_align bounds */

/*
 * The arithmetic thresholds measured in the differential run. All of these
 * decline; what differs is WHERE, and getting that wrong is how the signed
 * overflow got in.
 */
static void
cases_factored_bounds (void)
{
	/* The largest factored offset that survives mono's encoding: 2^28 * -8 is
	 * exactly G_MININT32. */
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_offset, 3, G_MININT32 } };

		p.n = 0;
		i_u8 (&p, (guint8)(DW_CFA_offset | 3)); i_uleb (&p, 1u << 28);
		ok_case ("offset-factored-max-representable", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* One more, and the byte offset no longer fits in mono's gint32 val. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, (guint8)(DW_CFA_offset | 3)); i_uleb (&p, (1u << 28) + 1);
		decline_case ("offset-factored-past-int32-declines", &p, 16);
	}
	/* Exactly the range check's bound: |factored| == 2^31 passes the check
	 * (it is a strict >), and is then refused by the caller's own window. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, (guint8)(DW_CFA_offset | 3)); i_uleb (&p, 1ull << 31);
		decline_case ("offset-factored-at-scale-limit-declines", &p, 16);
	}
	/* One past the bound: refused by scale_by_data_align() itself. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, (guint8)(DW_CFA_offset | 3)); i_uleb (&p, (1ull << 31) + 1);
		decline_case ("offset-factored-past-scale-limit-declines", &p, 16);
	}
	/*
	 * The divergence BOUNDARY: |factored| = (2^64 - 2^31)/8 = 2^61 - 2^28. At
	 * data_align = -8 the old unchecked multiply wrapped -(2^61 - 2^28) * -8
	 * back to exactly -2^31 = G_MININT32, the bottom of the offset forms'
	 * accepted window, so the old code ACCEPTED it through undefined behaviour.
	 * This is the smallest such operand: one less in magnitude wraps to
	 * -2^31 - 8, which is outside the window and declined either way.
	 */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_offset_extended_sf); i_uleb (&p, 3);
		i_sleb (&p, -DIVERGENCE_BOUNDARY);
		decline_case ("offset-divergence-boundary-negative-declines", &p, 16);
	}
	/* One below the boundary: outside every window under either arithmetic. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_offset_extended_sf); i_uleb (&p, 3);
		i_sleb (&p, -(DIVERGENCE_BOUNDARY - 1));
		decline_case ("offset-below-divergence-boundary-declines", &p, 16);
	}
	/* The positive sign never wrapped into the offset forms' window at all. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_offset_extended_sf); i_uleb (&p, 3);
		i_sleb (&p, DIVERGENCE_BOUNDARY);
		decline_case ("offset-divergence-boundary-positive-declines", &p, 16);
	}
	/*
	 * Well past the boundary: 2^61 - 3 wraps to -24, which is also inside the
	 * window. Kept because it is the value the project notes long recorded as
	 * "the smallest operand observed to change a verdict" - it is a diverging
	 * operand, but it is 268,435,453 above the real boundary above.
	 */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_offset_extended_sf); i_uleb (&p, 3);
		i_sleb (&p, -2305843009213693949LL);
		decline_case ("offset-wrapping-to-minus-24-declines", &p, 16);
	}
	/* And the extremes of the signed range. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_offset_extended_sf); i_uleb (&p, 3);
		i_sleb (&p, G_MININT64);
		decline_case ("offset-factored-int64-min-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_offset_extended_sf); i_uleb (&p, 3);
		i_sleb (&p, G_MAXINT64);
		decline_case ("offset-factored-int64-max-declines", &p, 16);
	}
	/* The same bound reached through the CFA _sf forms, whose accepted window
	 * is the other one: [0, G_MAXINT32]. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset_sf); i_sleb (&p, -((1LL << 31) + 1));
		decline_case ("def-cfa-offset-sf-past-scale-limit-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_sf); i_uleb (&p, 6);
		i_sleb (&p, -DIVERGENCE_BOUNDARY);
		decline_case ("def-cfa-sf-divergence-boundary-declines", &p, 16);
	}
}

/* ------------------------------------------------------- advance_loc forms */

static void
cases_advance_loc (void)
{
	/* The primary opcode, smallest and largest 6-bit delta. */
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 1, DW_CFA_def_cfa_offset, -1, 16 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc | 1);
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, 16);
		ok_case ("advance-loc-1", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 63, DW_CFA_def_cfa_offset, -1, 16 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc | 63);
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, 16);
		ok_case ("advance-loc-63", &p, 64, exp, G_N_ELEMENTS (exp));
	}
	/* Landing exactly on the last byte of the function is legal. */
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 16, DW_CFA_def_cfa_offset, -1, 16 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc | 16);
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, 16);
		ok_case ("advance-loc-to-exact-code-end", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* One byte past it is not. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc | 17);
		decline_case ("advance-loc-one-past-code-end-declines", &p, 16);
	}
	/* advance_loc1 / 2 / 4, at their operand maxima. */
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 255, DW_CFA_def_cfa_offset, -1, 16 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc1); i_u8 (&p, 255);
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, 16);
		ok_case ("advance-loc1-255", &p, 256, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 65535, DW_CFA_def_cfa_offset, -1, 16 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc2); i_u8 (&p, 0xff); i_u8 (&p, 0xff);
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, 16);
		ok_case ("advance-loc2-65535", &p, 65536, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0x10000, DW_CFA_def_cfa_offset, -1, 16 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc4);
		i_u8 (&p, 0x00); i_u8 (&p, 0x00); i_u8 (&p, 0x01); i_u8 (&p, 0x00);
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, 16);
		ok_case ("advance-loc4-65536", &p, 0x20000, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc1); i_u8 (&p, 255);
		decline_case ("advance-loc1-past-code-end-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc2); i_u8 (&p, 0xff); i_u8 (&p, 0xff);
		decline_case ("advance-loc2-past-code-end-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc4);
		i_u8 (&p, 0xff); i_u8 (&p, 0xff); i_u8 (&p, 0xff); i_u8 (&p, 0x7f);
		decline_case ("advance-loc4-past-code-end-declines", &p, 16);
	}
	/*
	 * delta * code_align is computed unsigned precisely so a huge product
	 * cannot be undefined; the loc_ > code_len check is what refuses it. This
	 * is the input that wraps a 32-bit product back to a small value.
	 */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc4);
		i_u8 (&p, 0xff); i_u8 (&p, 0xff); i_u8 (&p, 0xff); i_u8 (&p, 0xff);
		decline_case ("advance-loc4-max-uint32-declines", &p, 16);
	}
	/* code_align is applied to every delta. */
	{
		CieSpec s;
		Insn cie, p;
		ExpOp exp [] = { PROLOGUE_OPS, { 8, DW_CFA_def_cfa_offset, -1, 16 } };

		cie_spec_init (&s);
		s.code_align = 4;
		std_cie_insns (&cie);
		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc | 2);
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, 16);
		ok_case_cie ("advance-loc-code-align-4", &s, &cie, &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* set_loc is absolute; the transcoder tracks offsets only. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_set_loc);
		i_u8 (&p, 0); i_u8 (&p, 0); i_u8 (&p, 0); i_u8 (&p, 0);
		i_u8 (&p, 0); i_u8 (&p, 0); i_u8 (&p, 0); i_u8 (&p, 0);
		decline_case ("set-loc-declines", &p, 16);
	}
}

/* ------------------------------------------------------------- def_cfa forms */

static void
cases_def_cfa (void)
{
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa, 6, 16 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa); i_uleb (&p, 6); i_uleb (&p, 16);
		ok_case ("def-cfa", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_register, 6, 0 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_register); i_uleb (&p, 6);
		ok_case ("def-cfa-register", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_offset, -1, 32 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, 32);
		ok_case ("def-cfa-offset", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* The signed/factored forms: the operand is scaled by data_align (-8). */
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa, 6, 16 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_sf); i_uleb (&p, 6); i_sleb (&p, -2);
		ok_case ("def-cfa-sf", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_offset, -1, 32 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset_sf); i_sleb (&p, -4);
		ok_case ("def-cfa-offset-sf", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* mono encodes the CFA offset unsigned, so a negative one cannot survive. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_sf); i_uleb (&p, 6); i_sleb (&p, 2);
		decline_case ("def-cfa-sf-negative-cfa-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset_sf); i_sleb (&p, 4);
		decline_case ("def-cfa-offset-sf-negative-cfa-declines", &p, 16);
	}
	/* G_MAXINT32 is the largest CFA offset mono's gint32 val can carry. */
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_offset, -1, G_MAXINT32 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, G_MAXINT32);
		ok_case ("def-cfa-offset-max-int32", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, (guint64) G_MAXINT32 + 1);
		decline_case ("def-cfa-offset-past-int32-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa); i_uleb (&p, 6); i_uleb (&p, (guint64) G_MAXINT32 + 1);
		decline_case ("def-cfa-offset-operand-past-int32-declines", &p, 16);
	}
	/* Registers mono does not map, through each opcode that takes one. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa); i_uleb (&p, 17); i_uleb (&p, 16);
		decline_case ("def-cfa-register-out-of-range-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_register); i_uleb (&p, 200);
		decline_case ("def-cfa-register-opcode-out-of-range-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_sf); i_uleb (&p, 200); i_sleb (&p, -2);
		decline_case ("def-cfa-sf-register-out-of-range-declines", &p, 16);
	}
	/* Opcodes that carry no rule are skipped, not refused. */
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS };

		p.n = 0;
		i_u8 (&p, DW_CFA_nop); i_u8 (&p, DW_CFA_nop);
		i_u8 (&p, DW_CFA_GNU_args_size); i_uleb (&p, 64);
		ok_case ("nop-and-gnu-args-size-skipped", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* Expressions would need a DWARF expression evaluator in async-signal
	 * context; all three forms decline. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_expression); i_uleb (&p, 1); i_u8 (&p, 0x9c);
		decline_case ("def-cfa-expression-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_expression); i_uleb (&p, 3); i_uleb (&p, 1); i_u8 (&p, 0x9c);
		decline_case ("expression-declines", &p, 16);
	}
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_val_expression); i_uleb (&p, 3); i_uleb (&p, 1); i_u8 (&p, 0x9c);
		decline_case ("val-expression-declines", &p, 16);
	}
	/* An opcode nothing defines. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, 0x1d);
		decline_case ("unknown-opcode-declines", &p, 16);
	}
	/* DW_CFA_register (reg saved in another reg) is not representable. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_register); i_uleb (&p, 3); i_uleb (&p, 6);
		decline_case ("register-rule-declines", &p, 16);
	}
	/* val_offset is likewise unrepresentable. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_val_offset); i_uleb (&p, 3); i_uleb (&p, 1);
		decline_case ("val-offset-declines", &p, 16);
	}
}

/* --------------------------------------------- remember_state/restore_state */

static void
cases_remember_restore (void)
{
	/* One level: the saved CFA is materialized as a def_cfa, and neither
	 * opcode survives into the op list. */
	{
		Insn p;
		ExpOp exp [] = {
			PROLOGUE_OPS,
			{ 1, DW_CFA_def_cfa_offset, -1, 16 },
			{ 2, DW_CFA_def_cfa,        7,  8 },
		};

		p.n = 0;
		i_u8 (&p, DW_CFA_remember_state);
		i_u8 (&p, DW_CFA_advance_loc | 1);
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, 16);
		i_u8 (&p, DW_CFA_advance_loc | 1);
		i_u8 (&p, DW_CFA_restore_state);
		ok_case ("remember-restore-depth-1", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* A restored REGISTER rule is materialized too - as an offset when the
	 * saved state had one... */
	{
		Insn p;
		ExpOp exp [] = {
			PROLOGUE_OPS,
			{ 0, DW_CFA_offset,     3, -16 },
			{ 1, DW_CFA_same_value, 3, 0 },
			{ 2, DW_CFA_offset,     3, -16 },
		};

		p.n = 0;
		i_u8 (&p, (guint8)(DW_CFA_offset | 3)); i_uleb (&p, 2);
		i_u8 (&p, DW_CFA_remember_state);
		i_u8 (&p, DW_CFA_advance_loc | 1);
		i_u8 (&p, DW_CFA_same_value); i_uleb (&p, 3);
		i_u8 (&p, DW_CFA_advance_loc | 1);
		i_u8 (&p, DW_CFA_restore_state);
		ok_case ("restore-state-materializes-offset-rule", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* ...and as same_value when it had none. */
	{
		Insn p;
		ExpOp exp [] = {
			PROLOGUE_OPS,
			{ 1, DW_CFA_offset,     3, -16 },
			{ 2, DW_CFA_same_value, 3, 0 },
		};

		p.n = 0;
		i_u8 (&p, DW_CFA_remember_state);
		i_u8 (&p, DW_CFA_advance_loc | 1);
		i_u8 (&p, (guint8)(DW_CFA_offset | 3)); i_uleb (&p, 2);
		i_u8 (&p, DW_CFA_advance_loc | 1);
		i_u8 (&p, DW_CFA_restore_state);
		ok_case ("restore-state-materializes-same-value", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* An unmatched restore_state has nothing to restore. */
	{
		Insn p;

		p.n = 0;
		i_u8 (&p, DW_CFA_restore_state);
		decline_case ("restore-state-without-remember-declines", &p, 16);
	}
	/* The depth boundary is exact: MAX_REMEMBER_DEPTH pushes are accepted... */
	{
		Insn p;
		int i;

		p.n = 0;
		for (i = 0; i < EH_MAX_REMEMBER_DEPTH; ++i)
			i_u8 (&p, DW_CFA_remember_state);
		accept_n_case ("remember-depth-128-accepted", &p, 16, 2);
	}
	/* ...and one more is not. */
	{
		Insn p;
		int i;

		p.n = 0;
		for (i = 0; i < EH_MAX_REMEMBER_DEPTH + 1; ++i)
			i_u8 (&p, DW_CFA_remember_state);
		decline_case ("remember-depth-129-declines", &p, 16);
	}
	/* Popping all the way back from the maximum depth still works. */
	{
		Insn p;
		int i;

		p.n = 0;
		for (i = 0; i < EH_MAX_REMEMBER_DEPTH; ++i)
			i_u8 (&p, DW_CFA_remember_state);
		for (i = 0; i < EH_MAX_REMEMBER_DEPTH; ++i)
			i_u8 (&p, DW_CFA_restore_state);
		accept_n_case ("remember-restore-full-depth-round-trip", &p, 16, 2);
	}
	/*
	 * A restore_state whose saved state has no CFA rule. Only reachable in the
	 * CIE's initial instructions, where the state starts out with cfa_reg = -1:
	 * by the time the FDE program runs, transcode_fde() has already required a
	 * CFA rule. Emitting def_cfa here would fabricate a rule on r0.
	 */
	{
		CieSpec s;
		Insn cie, p;

		cie_spec_init (&s);
		cie.n = 0;
		i_u8 (&cie, DW_CFA_remember_state);
		i_u8 (&cie, DW_CFA_def_cfa); i_uleb (&cie, 7); i_uleb (&cie, 8);
		i_u8 (&cie, DW_CFA_restore_state);
		p.n = 0;
		i_u8 (&p, DW_CFA_nop);
		decline_case_cie ("cie-restore-state-without-cfa-declines", &s, &cie, &p, 16);
	}
}

/* ---------------------------------------------------- DW_CFA_restore family */

static void
cases_restore (void)
{
	/* restore_extended reaches the same rule as the primary opcode. */
	{
		Insn p;
		ExpOp exp [] = {
			PROLOGUE_OPS,
			{ 2, DW_CFA_offset, PC_REG, -16 },
			{ 4, DW_CFA_offset, PC_REG, -8 },
		};

		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc | 2);
		i_u8 (&p, (guint8)(DW_CFA_offset | PC_REG)); i_uleb (&p, 2);
		i_u8 (&p, DW_CFA_advance_loc | 2);
		i_u8 (&p, DW_CFA_restore_extended); i_uleb (&p, (guint64) PC_REG);
		ok_case ("restore-extended-to-cie-rule", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* DW_CFA_restore inside the CIE has no initial rules to revert to. */
	{
		CieSpec s;
		Insn cie, p;

		cie_spec_init (&s);
		std_cie_insns (&cie);
		i_u8 (&cie, (guint8)(DW_CFA_restore | 3));
		p.n = 0;
		i_u8 (&p, DW_CFA_nop);
		decline_case_cie ("cie-restore-declines", &s, &cie, &p, 16);
	}
	{
		CieSpec s;
		Insn cie, p;

		cie_spec_init (&s);
		std_cie_insns (&cie);
		i_u8 (&cie, DW_CFA_restore_extended); i_uleb (&cie, 3);
		p.n = 0;
		i_u8 (&p, DW_CFA_nop);
		decline_case_cie ("cie-restore-extended-declines", &s, &cie, &p, 16);
	}
	{
		CieSpec s;
		Insn cie, p;

		cie_spec_init (&s);
		std_cie_insns (&cie);
		i_u8 (&cie, DW_CFA_set_loc);
		i_u8 (&cie, 0); i_u8 (&cie, 0); i_u8 (&cie, 0); i_u8 (&cie, 0);
		i_u8 (&cie, 0); i_u8 (&cie, 0); i_u8 (&cie, 0); i_u8 (&cie, 0);
		p.n = 0;
		i_u8 (&p, DW_CFA_nop);
		decline_case_cie ("cie-set-loc-declines", &s, &cie, &p, 16);
	}
}

/* ------------------------------------------------------------- LEB128 forms */

/*
 * The operand decoders are the part of this file that is handed attacker-shaped
 * bytes, so each of their exits gets a case: minimal encodings, redundant
 * multi-byte ones, the maximal 64-bit ones, and the two ways an overlong
 * encoding is refused (significant bits past bit 63, and a shift that runs past
 * the clamp).
 */
static void
cases_leb128 (void)
{
	/* uleb: minimal, redundant multi-byte, and the same value both ways. */
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_offset, -1, 8 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset); i_u8 (&p, 0x08);
		ok_case ("uleb-minimal", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_offset, -1, 8 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset); i_u8 (&p, 0x88); i_u8 (&p, 0x00);
		ok_case ("uleb-redundant-two-byte", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_offset, -1, 16383 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset); i_u8 (&p, 0xff); i_u8 (&p, 0x7f);
		ok_case ("uleb-two-byte-max", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_offset, -1, G_MAXINT32 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset);
		i_u8 (&p, 0xff); i_u8 (&p, 0xff); i_u8 (&p, 0xff); i_u8 (&p, 0xff); i_u8 (&p, 0x07);
		ok_case ("uleb-five-byte-int32-max", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* The maximal legal uleb128 for a 64-bit value: ten bytes, last one 0x01.
	 * It decodes (this is not a decoder failure) and is then out of range. */
	{
		Insn p;
		int i;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset);
		for (i = 0; i < 9; ++i)
			i_u8 (&p, 0xff);
		i_u8 (&p, 0x01);
		decline_case ("uleb-ten-byte-uint64-max-declines", &p, 16);
	}
	/* Eleven bytes with significant bits past bit 63: refused by the decoder. */
	{
		Insn p;
		int i;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset);
		for (i = 0; i < 10; ++i)
			i_u8 (&p, 0xff);
		i_u8 (&p, 0x7f);
		decline_case ("uleb-overlong-significant-bits-declines", &p, 16);
	}
	/* Eleven zero-payload continuation bytes: refused by the shift clamp. */
	{
		Insn p;
		int i;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset);
		for (i = 0; i < 10; ++i)
			i_u8 (&p, 0xff);
		i_u8 (&p, 0x80); i_u8 (&p, 0x00);
		decline_case ("uleb-overlong-shift-clamp-declines", &p, 16);
	}
	/*
	 * The two cases above encode enormous values, so they would decline on
	 * range even if the decoder let them through. These two encode 8, and
	 * decline for one reason only: the decoder refused the encoding. Without
	 * them, loosening either guard (shift clamp, or the reject for significant
	 * bits above bit 63) is invisible to this file.
	 */
	{
		Insn p;
		int i;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset);
		i_u8 (&p, 0x88);                 /* payload 8, continues */
		for (i = 0; i < 10; ++i)
			i_u8 (&p, 0x80);         /* ten zero payloads, shift runs to 77 */
		i_u8 (&p, 0x00);
		decline_case ("uleb-twelve-byte-encoding-of-8-declines", &p, 16);
	}
	{
		Insn p;
		int i;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset);
		i_u8 (&p, 0x88);                 /* payload 8, continues */
		for (i = 0; i < 9; ++i)
			i_u8 (&p, 0x80);
		i_u8 (&p, 0x7f);                 /* significant bits at shift 70 */
		decline_case ("uleb-high-bits-above-bit-63-over-8-declines", &p, 16);
	}
	/*
	 * A nine-byte sleb128 puts its last operand byte at shift 56, so the sign
	 * bit is tested at shift 63 - the largest shift at which sign extension
	 * still happens. Encodes -8, which scales to a CFA offset of +64.
	 */
	{
		Insn p;
		int i;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_offset, -1, 64 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset_sf);
		i_u8 (&p, 0xf8);
		for (i = 0; i < 7; ++i)
			i_u8 (&p, 0xff);
		i_u8 (&p, 0x7f);
		ok_case ("sleb-nine-byte-sign-extension-at-shift-63", &p, 16, exp, G_N_ELEMENTS (exp));
	}

	/* sleb: minimal negative, redundant multi-byte, sign extension. */
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_offset, -1, 16 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset_sf); i_u8 (&p, 0x7e); /* -2 */
		ok_case ("sleb-minimal-negative", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_offset, -1, 32 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset_sf);
		i_u8 (&p, 0xfc); i_u8 (&p, 0x7f); /* -4, redundantly */
		ok_case ("sleb-redundant-two-byte-negative", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	{
		Insn p;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_offset, -1, 8 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset_sf); i_u8 (&p, 0x7f); /* -1 */
		ok_case ("sleb-minus-one", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/*
	 * An overlong sleb whose extra bytes are pure sign extension (0x7f payload
	 * past bit 63) is well-defined and must still decode to -1: the decoder
	 * accepts 0x7f above the clamp precisely because that is sign, not value.
	 */
	{
		Insn p;
		int i;
		ExpOp exp [] = { PROLOGUE_OPS, { 0, DW_CFA_def_cfa_offset, -1, 8 } };

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset_sf);
		for (i = 0; i < 10; ++i)
			i_u8 (&p, 0xff);
		i_u8 (&p, 0x7f);
		ok_case ("sleb-overlong-sign-extension-accepted", &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* Significant (non-sign) bits past bit 63 are refused. */
	{
		Insn p;
		int i;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset_sf);
		for (i = 0; i < 10; ++i)
			i_u8 (&p, 0xff);
		i_u8 (&p, 0x3f);
		decline_case ("sleb-overlong-significant-bits-declines", &p, 16);
	}
	/* And so is a shift that runs past the clamp. */
	{
		Insn p;
		int i;

		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset_sf);
		for (i = 0; i < 10; ++i)
			i_u8 (&p, 0xff);
		i_u8 (&p, 0xff); i_u8 (&p, 0x00);
		decline_case ("sleb-overlong-shift-clamp-declines", &p, 16);
	}
	/*
	 * A truncated operand: the opcode is the last byte of the FDE, so the
	 * uleb128 that follows it is not there. The entry is built without its
	 * alignment padding, or a nop would stand in for the missing operand.
	 */
	{
		EhFrame e;
		Insn p;

		memset (&e, 0, sizeof (e));
		std_cie (&e);
		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset);
		build_fde_ex (&e, 16, p.b, p.n, FALSE);
		expect_decline ("uleb-operand-truncated-declines", &e, 16);
	}
	{
		EhFrame e;
		Insn p;

		memset (&e, 0, sizeof (e));
		std_cie (&e);
		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset_sf);
		build_fde_ex (&e, 16, p.b, p.n, FALSE);
		expect_decline ("sleb-operand-truncated-declines", &e, 16);
	}
	/* An unterminated LEB128 at the end of the entry. */
	{
		EhFrame e;
		Insn p;

		memset (&e, 0, sizeof (e));
		std_cie (&e);
		p.n = 0;
		i_u8 (&p, DW_CFA_def_cfa_offset); i_u8 (&p, 0x80); /* continuation, then nothing */
		build_fde_ex (&e, 16, p.b, p.n, FALSE);
		expect_decline ("uleb-unterminated-declines", &e, 16);
	}
	/*
	 * One case per opcode whose operand can run off the end of the entry.
	 * Each of these lands on that opcode's own "the reader already failed"
	 * guard, which returns without acting on a zero operand; the decline then
	 * comes from the reader check after the interpreter loop.
	 */
	{
		static const struct { const char *name; guint8 op; gboolean partial; } tab [] = {
			{ "def-cfa-operand-truncated-declines",             DW_CFA_def_cfa,            TRUE },
			{ "def-cfa-register-operand-truncated-declines",    DW_CFA_def_cfa_register,   FALSE },
			{ "def-cfa-sf-operand-truncated-declines",          DW_CFA_def_cfa_sf,         TRUE },
			{ "offset-extended-operand-truncated-declines",     DW_CFA_offset_extended,    TRUE },
			{ "offset-extended-sf-operand-truncated-declines",  DW_CFA_offset_extended_sf, TRUE },
			{ "restore-extended-operand-truncated-declines",    DW_CFA_restore_extended,   FALSE },
			{ "same-value-operand-truncated-declines",          DW_CFA_same_value,         FALSE },
			{ "undefined-operand-truncated-declines",           DW_CFA_undefined,          FALSE },
			{ "offset-operand-truncated-declines",              DW_CFA_offset | 3,         FALSE },
			{ "gnu-args-size-operand-truncated-declines",       DW_CFA_GNU_args_size,      FALSE },
		};
		int k;

		for (k = 0; k < (int) G_N_ELEMENTS (tab); ++k) {
			EhFrame e;
			Insn p;

			memset (&e, 0, sizeof (e));
			std_cie (&e);
			p.n = 0;
			i_u8 (&p, tab [k].op);
			/* Opcodes with two operands get the first one, so the truncation
			 * is in the second. */
			if (tab [k].partial)
				i_u8 (&p, 3);
			build_fde_ex (&e, 16, p.b, p.n, FALSE);
			expect_decline (tab [k].name, &e, 16);
		}
	}
	/* The fixed-width operands truncate too. */
	{
		EhFrame e;
		Insn p;

		memset (&e, 0, sizeof (e));
		std_cie (&e);
		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc2); i_u8 (&p, 0x01); /* second byte missing */
		build_fde_ex (&e, 16, p.b, p.n, FALSE);
		expect_decline ("advance-loc2-operand-truncated-declines", &e, 16);
	}
	{
		EhFrame e;
		Insn p;

		memset (&e, 0, sizeof (e));
		std_cie (&e);
		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc4); i_u8 (&p, 0x01); i_u8 (&p, 0x00);
		build_fde_ex (&e, 16, p.b, p.n, FALSE);
		expect_decline ("advance-loc4-operand-truncated-declines", &e, 16);
	}
	{
		EhFrame e;
		Insn p;

		memset (&e, 0, sizeof (e));
		std_cie (&e);
		p.n = 0;
		i_u8 (&p, DW_CFA_advance_loc1); /* one-byte operand missing */
		build_fde_ex (&e, 16, p.b, p.n, FALSE);
		expect_decline ("advance-loc1-operand-truncated-declines", &e, 16);
	}
}

/* ------------------------------------------------------------ MAX_UNWIND_OPS */

static void
cases_op_limit (void)
{
	Insn p;
	int i;

	/*
	 * mono_unwind_ops_encode_full() writes into a fixed guint8 buf[4096] and
	 * checks the bound only after the writes, so the list length is capped
	 * here. The two ops restating the CIE count towards it.
	 */
	p.n = 0;
	for (i = 0; i < EH_MAX_UNWIND_OPS - 2; ++i) {
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, 16 + (i % 8) * 8);
	}
	accept_n_case ("op-count-exactly-128-accepted", &p, 16, EH_MAX_UNWIND_OPS);

	/* One more op, and the list is refused - by the check after the run, since
	 * the loop's own check is made before each opcode. */
	p.n = 0;
	for (i = 0; i < EH_MAX_UNWIND_OPS - 1; ++i) {
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, 16 + (i % 8) * 8);
	}
	decline_case ("op-count-129-declines", &p, 16);

	/* Far past it, so the check inside the run loop is the one that fires. */
	p.n = 0;
	for (i = 0; i < 300; ++i) {
		i_u8 (&p, DW_CFA_def_cfa_offset); i_uleb (&p, 16 + (i % 8) * 8);
	}
	decline_case ("op-count-302-declines-mid-run", &p, 16);
}

/* ------------------------------------------------------------- CIE variants */

static void
cases_cie (void)
{
	CieSpec s;
	Insn cie, p;

	std_cie_insns (&cie);
	p.n = 0;
	i_u8 (&p, DW_CFA_nop);

	/* Version 3 puts the return column in a uleb128 rather than a byte. */
	cie_spec_init (&s);
	s.version = 3;
	{
		ExpOp exp [] = { PROLOGUE_OPS };
		ok_case_cie ("cie-version-3", &s, &cie, &p, 16, exp, G_N_ELEMENTS (exp));
	}

	cie_spec_init (&s); s.version = 2;
	decline_case_cie ("cie-version-2-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.version = 0;
	decline_case_cie ("cie-version-0-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.version = 4;
	decline_case_cie ("cie-version-4-declines", &s, &cie, &p, 16);

	/* No augmentation at all: FDE pointers are then absolute. */
	cie_spec_init (&s); s.aug = "";
	{
		ExpOp exp [] = { PROLOGUE_OPS };
		ok_case_cie ("cie-no-augmentation-absolute-fde-pointer", &s, &cie, &p, 16,
		             exp, G_N_ELEMENTS (exp));
	}
	/* An augmentation with no length field cannot be skipped safely. */
	cie_spec_init (&s); s.aug = "R";
	decline_case_cie ("cie-augmentation-without-z-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.aug = "zRRRRRRRRRRRRRRRRRR";
	decline_case_cie ("cie-augmentation-too-long-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.aug = "zX";
	decline_case_cie ("cie-augmentation-unknown-letter-declines", &s, &cie, &p, 16);
	/*
	 * The augmentation-length bound is 16 characters, exactly. Both sides use
	 * only letters that carry no data ('S'), so the 17-character one is refused
	 * by the LENGTH bound and nothing else - which is what makes the pair pin
	 * the bound rather than the letter handling.
	 */
	cie_spec_init (&s); s.aug = "zRSSSSSSSSSSSSSS";                 /* 16 */
	{
		ExpOp exp [] = { PROLOGUE_OPS };
		ok_case_cie ("cie-augmentation-16-characters", &s, &cie, &p, 16,
		             exp, G_N_ELEMENTS (exp));
	}
	cie_spec_init (&s); s.aug = "zRSSSSSSSSSSSSSSS";                /* 17 */
	decline_case_cie ("cie-augmentation-17-characters-declines", &s, &cie, &p, 16);

	/* The augmentations LLVM and GCC actually emit alongside 'R'. */
	cie_spec_init (&s); s.aug = "zPR";
	{
		ExpOp exp [] = { PROLOGUE_OPS };
		ok_case_cie ("cie-augmentation-personality", &s, &cie, &p, 16, exp, G_N_ELEMENTS (exp));
	}
	cie_spec_init (&s); s.aug = "zPLR";
	{
		ExpOp exp [] = { PROLOGUE_OPS };
		ok_case_cie ("cie-augmentation-personality-and-lsda", &s, &cie, &p, 16,
		             exp, G_N_ELEMENTS (exp));
	}
	cie_spec_init (&s); s.aug = "zSR";
	{
		ExpOp exp [] = { PROLOGUE_OPS };
		ok_case_cie ("cie-augmentation-signal-frame", &s, &cie, &p, 16, exp, G_N_ELEMENTS (exp));
	}
	/* A personality encoding whose width is not known cannot be skipped. */
	cie_spec_init (&s); s.aug = "zPR"; s.personality_enc = PE_uleb128;
	decline_case_cie ("cie-personality-unskippable-encoding-declines", &s, &cie, &p, 16);
	/*
	 * A declared augmentation length that runs off the end of the CIE.
	 *
	 * This pins the OUTCOME, not the check: CieInfo::parse()'s explicit
	 * "aug_len > r.remaining ()" test is not load-bearing on its own. Delete it
	 * and this input still declines, because r.seek (aug_end) then range-checks
	 * the same thing (that is also why Reader::seek()'s failure path is dead in
	 * the shipped code - see the coverage notes in the commit message).
	 */
	cie_spec_init (&s); s.aug_len_delta = 200;
	decline_case_cie ("cie-augmentation-length-past-entry-end-declines", &s, &cie, &p, 16);

	/* The alignments and the return column must be the ones mono re-factors
	 * with, or offsets silently shift and the unwound frame resumes wrong. */
	cie_spec_init (&s); s.data_align = -4;
	decline_case_cie ("cie-data-align-mismatch-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.data_align = 8;
	decline_case_cie ("cie-data-align-positive-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.data_align = 0;
	decline_case_cie ("cie-data-align-zero-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.code_align = 0;
	decline_case_cie ("cie-code-align-zero-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.return_reg = 15;
	decline_case_cie ("cie-return-column-mismatch-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.version = 3; s.return_reg = 300;
	decline_case_cie ("cie-return-column-out-of-range-declines", &s, &cie, &p, 16);

	/*
	 * The same three fields, given 64-bit operands whose low 32 bits ALIAS the
	 * accepted values. code_align/data_align/return_reg are int fields, and
	 * every check on them runs after the narrowing, so before CieInfo::parse()
	 * range-checked the wire values these four were ACCEPTED - a header-field
	 * check defeated by a cast, on input parsed as untrusted. They must decline.
	 */
	cie_spec_init (&s); s.data_align = 4294967288LL;         /* 2^32 - 8 -> -8 */
	decline_case_cie ("cie-data-align-aliasing-minus-8-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.data_align = -8LL - 4294967296LL;  /* -(2^32 + 8) -> -8 */
	decline_case_cie ("cie-data-align-below-int32-aliasing-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.code_align = 4294967297ULL;        /* 2^32 + 1 -> 1 */
	decline_case_cie ("cie-code-align-aliasing-1-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.version = 3; s.return_reg = 4294967312ULL; /* 2^32 + 16 -> 16 */
	decline_case_cie ("cie-return-column-aliasing-16-declines", &s, &cie, &p, 16);
	/*
	 * The largest value the range check lets through. It is refused one step
	 * later, by the return-column comparison - which is the point: the range
	 * check exists to make sure that comparison sees the value that was on the
	 * wire, not the low 32 bits of it.
	 */
	cie_spec_init (&s); s.version = 3; s.return_reg = (guint64) G_MAXINT32;
	decline_case_cie ("cie-return-column-int32-max-declines", &s, &cie, &p, 16);

	/* A CIE that establishes no CFA rule leaves nothing to unwind with. */
	cie_spec_init (&s);
	{
		Insn c2;

		c2.n = 0;
		i_u8 (&c2, (guint8)(DW_CFA_offset | PC_REG)); i_uleb (&c2, 1);
		decline_case_cie ("cie-without-cfa-rule-declines", &s, &c2, &p, 16);
	}
	/* A CIE whose own initial instructions run past the end of the function. */
	cie_spec_init (&s);
	{
		Insn c2;

		c2.n = 0;
		i_u8 (&c2, DW_CFA_def_cfa); i_uleb (&c2, 7); i_uleb (&c2, 8);
		i_u8 (&c2, DW_CFA_advance_loc1); i_u8 (&c2, 255);
		decline_case_cie ("cie-advance-past-code-end-declines", &s, &c2, &p, 16);
	}
}

/* -------------------------------------------------------- FDE pointer forms */

static void
cases_fde_pointer_encodings (void)
{
	CieSpec s;
	Insn cie, p;
	ExpOp exp [] = { PROLOGUE_OPS };

	std_cie_insns (&cie);
	p.n = 0;
	i_u8 (&p, DW_CFA_nop);

	/* What LLVM emits on x86-64, plus the other widths the decoder accepts. */
	cie_spec_init (&s); s.fde_ptr_enc = PE_pcrel | PE_sdata4;
	ok_case_cie ("fde-pointer-pcrel-sdata4", &s, &cie, &p, 16, exp, G_N_ELEMENTS (exp));
	cie_spec_init (&s); s.fde_ptr_enc = PE_pcrel | PE_sdata8;
	ok_case_cie ("fde-pointer-pcrel-sdata8", &s, &cie, &p, 16, exp, G_N_ELEMENTS (exp));
	cie_spec_init (&s); s.fde_ptr_enc = PE_pcrel | PE_udata4;
	ok_case_cie ("fde-pointer-pcrel-udata4", &s, &cie, &p, 16, exp, G_N_ELEMENTS (exp));
	cie_spec_init (&s); s.fde_ptr_enc = PE_pcrel | PE_udata8;
	ok_case_cie ("fde-pointer-pcrel-udata8", &s, &cie, &p, 16, exp, G_N_ELEMENTS (exp));
	cie_spec_init (&s); s.fde_ptr_enc = PE_absptr;
	ok_case_cie ("fde-pointer-absptr", &s, &cie, &p, 16, exp, G_N_ELEMENTS (exp));

	/* DW_EH_PE_indirect would yield the address OF the pointer. */
	cie_spec_init (&s); s.fde_ptr_enc = PE_indirect | PE_pcrel | PE_sdata4;
	decline_case_cie ("fde-pointer-indirect-declines", &s, &cie, &p, 16);
	/* A relocation base the transcoder has no way to know. */
	cie_spec_init (&s); s.fde_ptr_enc = PE_datarel | PE_sdata4;
	decline_case_cie ("fde-pointer-datarel-declines", &s, &cie, &p, 16);
	/* Widths the decoder does not handle. */
	cie_spec_init (&s); s.fde_ptr_enc = PE_pcrel | PE_sdata2;
	decline_case_cie ("fde-pointer-sdata2-declines", &s, &cie, &p, 16);
	cie_spec_init (&s); s.fde_ptr_enc = PE_pcrel | PE_uleb128;
	decline_case_cie ("fde-pointer-uleb128-declines", &s, &cie, &p, 16);

	/* An FDE for some other function is not a failure: the scan continues. It
	 * is also what happens when the code address simply is not described. */
	{
		EhFrame e;
		GSList *ops = NULL;

		memset (&e, 0, sizeof (e));
		std_cie (&e);
		build_fde (&e, 16, p.b, p.n);
		current_case = "fde-for-other-function-declines";
		cases_run ++;
		if (mono_llvm_eh_frame_to_unwind_ops (e.buf, (guint32) e.len,
						      (gpointer)(e.buf + 0x999), 16, &ops)) {
			printf ("FAIL fde-for-other-function-declines: expected decline\n");
			failures ++;
			mono_free_unwind_info (ops);
		} else {
			printf ("ok   fde-for-other-function-declines (declined)\n");
		}
	}
	/* Two FDEs, the second one ours: the scan must step over the first. */
	{
		EhFrame e;
		GSList *ops = NULL;
		gpointer wanted;
		ExpOp exp2 [] = { PROLOGUE_OPS };

		memset (&e, 0, sizeof (e));
		std_cie (&e);
		build_fde (&e, 16, p.b, p.n);   /* FDE #1, at pcbegin + 0x100 */
		{
			/* FDE #2 describes a different address; make #1 the miss by
			 * asking for #2's. */
			build_fde (&e, 16, p.b, p.n);
			wanted = code_start_of (&e);
		}
		current_case = "fde-scan-skips-other-function";
		cases_run ++;
		if (!mono_llvm_eh_frame_to_unwind_ops (e.buf, (guint32) e.len, wanted, 16, &ops)) {
			printf ("FAIL fde-scan-skips-other-function: declined\n");
			failures ++;
		} else {
			check_ops ("fde-scan-skips-other-function", ops, exp2, G_N_ELEMENTS (exp2));
			cases_run --; /* check_ops counts it */
			mono_free_unwind_info (ops);
		}
	}
}

/* --------------------------------------------------- section-level rejection */

/*
 * These cannot go through the builders: they are about the framing itself, so
 * the bytes are patched or truncated after the fact.
 */
static void
cases_section_framing (void)
{
	EhFrame e;
	Insn cie, p;
	GSList *ops = NULL;

	std_cie_insns (&cie);
	p.n = 0;
	i_u8 (&p, DW_CFA_nop);

	/* A null section, and one too short to hold a length field. */
	current_case = "section-null-buffer-declines";
	cases_run ++;
	if (mono_llvm_eh_frame_to_unwind_ops (NULL, 0, (gpointer) 0x1000, 16, &ops) || ops) {
		printf ("FAIL section-null-buffer-declines\n"); failures ++;
	} else {
		printf ("ok   section-null-buffer-declines (declined)\n");
	}

	memset (&e, 0, sizeof (e));
	std_cie (&e);
	build_fde (&e, 16, p.b, p.n);
	current_case = "section-shorter-than-length-field-declines";
	cases_run ++;
	ops = NULL;
	if (mono_llvm_eh_frame_to_unwind_ops (e.buf, 3, code_start_of (&e), 16, &ops) || ops) {
		printf ("FAIL section-shorter-than-length-field-declines\n"); failures ++;
	} else {
		printf ("ok   section-shorter-than-length-field-declines (declined)\n");
	}

	/* No out parameter at all. */
	current_case = "section-null-out-ops-declines";
	cases_run ++;
	if (mono_llvm_eh_frame_to_unwind_ops (e.buf, (guint32) e.len, code_start_of (&e), 16, NULL)) {
		printf ("FAIL section-null-out-ops-declines\n"); failures ++;
	} else {
		printf ("ok   section-null-out-ops-declines (declined)\n");
	}

	/* A length field claiming more than the section holds. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	build_fde (&e, 16, p.b, p.n);
	current_case = "section-truncated-mid-fde-declines";
	cases_run ++;
	ops = NULL;
	if (mono_llvm_eh_frame_to_unwind_ops (e.buf, (guint32)(e.len - 6), code_start_of (&e), 16, &ops) || ops) {
		printf ("FAIL section-truncated-mid-fde-declines\n"); failures ++;
	} else {
		printf ("ok   section-truncated-mid-fde-declines (declined)\n");
	}

	/*
	 * 64-bit DWARF framing, which this transcoder declines rather than guess at.
	 *
	 * Named for the input, not for the branch: the dedicated "length ==
	 * 0xffffffff" return is not load-bearing for any section smaller than 4 GiB,
	 * because "length > sec.remaining ()" one line later refuses the same bytes.
	 * Nothing here can build a 4 GiB section, so this case pins the outcome only.
	 */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	build_fde (&e, 16, p.b, p.n);
	memcpy (e.buf, &(guint32){ 0xffffffff }, 4);
	expect_decline ("section-length-0xffffffff-declines", &e, 16);

	/* An entry whose length leaves no room for the id field. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	build_fde (&e, 16, p.b, p.n);
	memcpy (e.buf, &(guint32){ 1 }, 4);
	e.len = 5;
	expect_decline ("section-entry-too-short-for-id-declines", &e, 16);

	/* A zero-length entry terminates the section; with no FDE before it, there
	 * is nothing to describe the method. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	b_u32 (&e, 0);
	e.code_start = e.buf + 0x100;
	expect_decline ("section-zero-length-terminator-declines", &e, 16);

	/* A CIE and no FDE at all. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	e.code_start = e.buf + 0x100;
	expect_decline ("section-cie-without-fde-declines", &e, 16);

	/* An FDE whose CIE pointer reaches back before the section. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	build_fde (&e, 16, p.b, p.n);
	{
		guint32 cie_len;
		int fde_start;

		memcpy (&cie_len, e.buf, 4);
		fde_start = 4 + cie_len;
		memcpy (e.buf + fde_start + 4, &(guint32){ 0x40000000 }, 4);
		expect_decline ("fde-cie-pointer-before-section-declines", &e, 16);
	}

	/* An FDE whose CIE pointer lands on a zero length field. The CIE id field
	 * of the real CIE is four zero bytes, so point at that. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	build_fde (&e, 16, p.b, p.n);
	{
		guint32 cie_len;
		int fde_start;

		memcpy (&cie_len, e.buf, 4);
		fde_start = 4 + cie_len;
		/* The id field sits at offset 4; a CIE pointer of (fde_start + 4 - 4). */
		memcpy (e.buf + fde_start + 4, &(guint32){ (guint32)(fde_start + 4 - 4) }, 4);
		expect_decline ("fde-cie-pointer-to-zero-length-declines", &e, 16);
	}

	/* An FDE whose CIE pointer lands on a length field of 0xffffffff, and one
	 * that lands on a length larger than the rest of the section. Both byte
	 * patterns are planted in the CIE's own initial instructions, which the
	 * transcoder never interprets on this path. */
	{
		Insn c2;
		guint32 cie_len;
		int fde_start, planted;

		memset (&e, 0, sizeof (e));
		c2.n = 0;
		i_u8 (&c2, DW_CFA_def_cfa); i_uleb (&c2, 7); i_uleb (&c2, 8);
		i_u8 (&c2, DW_CFA_nop);  /* pad so the planted word is 4-aligned-ish */
		planted = -1;
		{
			CieSpec s;
			cie_spec_init (&s);
			/* four 0xff bytes, valid CFI (restore r63) but never run here */
			i_u8 (&c2, 0xff); i_u8 (&c2, 0xff); i_u8 (&c2, 0xff); i_u8 (&c2, 0xff);
			build_cie_spec (&e, &s, c2.b, c2.n);
		}
		/* Locate the planted word: it is the last four bytes before padding. */
		{
			int i;
			for (i = 0; i + 4 <= e.len; ++i) {
				if (e.buf [i] == 0xff && e.buf [i+1] == 0xff &&
				    e.buf [i+2] == 0xff && e.buf [i+3] == 0xff) {
					planted = i;
					break;
				}
			}
		}
		g_assert (planted >= 0);
		build_fde (&e, 16, p.b, p.n);
		memcpy (&cie_len, e.buf, 4);
		fde_start = 4 + cie_len;
		memcpy (e.buf + fde_start + 4, &(guint32){ (guint32)(fde_start + 4 - planted) }, 4);
		expect_decline ("fde-cie-pointer-to-64bit-length-declines", &e, 16);
	}
	{
		Insn c2;
		CieSpec s;
		guint32 cie_len;
		int fde_start, planted = -1, i;

		memset (&e, 0, sizeof (e));
		cie_spec_init (&s);
		c2.n = 0;
		i_u8 (&c2, DW_CFA_def_cfa); i_uleb (&c2, 7); i_uleb (&c2, 8);
		i_u8 (&c2, DW_CFA_nop);
		/* 0x40000000, far more than the section holds */
		i_u8 (&c2, 0x00); i_u8 (&c2, 0x00); i_u8 (&c2, 0x00); i_u8 (&c2, 0x40);
		build_cie_spec (&e, &s, c2.b, c2.n);
		for (i = 0; i + 4 <= e.len; ++i) {
			if (e.buf [i] == 0x00 && e.buf [i+1] == 0x00 &&
			    e.buf [i+2] == 0x00 && e.buf [i+3] == 0x40) {
				planted = i;
				break;
			}
		}
		g_assert (planted >= 0);
		build_fde (&e, 16, p.b, p.n);
		memcpy (&cie_len, e.buf, 4);
		fde_start = 4 + cie_len;
		memcpy (e.buf + fde_start + 4, &(guint32){ (guint32)(fde_start + 4 - planted) }, 4);
		expect_decline ("fde-cie-pointer-to-overlong-length-declines", &e, 16);
	}

	/*
	 * A CIE pointer to a length that overruns the section, where everything
	 * ELSE about the pointed-at CIE is valid.
	 *
	 * This is the case that pins locate()'s "cie_len > r.remaining ()" check.
	 * The two cases above do not: their planted CIEs are garbage, so parse()
	 * refuses them whether or not the length was checked, and the check can be
	 * deleted with them still green. Here parse() would SUCCEED - so with the
	 * check removed the FDE transcodes and this case fails, which is the point.
	 *
	 * The layout, all inside one buffer, with the section declared to end at E:
	 *
	 *   [ real CIE .... [fake CIE image at T] ][ FDE at F ] E [ nop tail ]
	 *                                                        ^ declared end
	 *
	 * The fake CIE's declared length reaches past E into the tail, which is the
	 * overrun. Its contents are a well-formed zR CIE whose CFI program starts at
	 * T+17 and runs to T+4+cie_len - i.e. over the rest of the real CIE, over
	 * the FDE's own bytes, and into the tail. Every one of those bytes has to be
	 * representable CFI, which is why the FDE is padded to a length whose low
	 * byte (0x41) and CIE-pointer byte (0x41) are both DW_CFA_advance_loc, its
	 * address_range is zeroed, and the tail is zero (DW_CFA_nop).
	 *
	 * Without the length check the CIE parses, the CIE program runs to the tail,
	 * the FDE program is 52 nops, and the transcode returns two ops. With it,
	 * locate() declines before any of that.
	 */
	{
		const int fake_hdr = 22;   /* T .. T+21 */
		const int cie_ptr = 0x41;  /* also DW_CFA_advance_loc | 1 */
		const int fde_len = 0x41;  /* ditto, as the FDE's length byte */
		int f, t, e_size, tail, cie_len, i;
		Insn c2;

		memset (&e, 0, sizeof (e));
		/* A real CIE with room for the fake image inside its instructions. */
		c2.n = 0;
		for (i = 0; i < 100; ++i)
			i_u8 (&c2, DW_CFA_nop);
		build_cie (&e, c2.b, c2.n);

		f = e.len;
		t = f + 4 - cie_ptr;
		g_assert (t > 0 && t + fake_hdr < f);

		/* The FDE: contents length must come out as fde_len exactly. */
		{
			Insn q;

			q.n = 0;
			for (i = 0; i < fde_len - 13; ++i)
				i_u8 (&q, DW_CFA_nop);
			build_fde_ex (&e, 0x1000, q.b, q.n, FALSE);
			g_assert (e.len - f - 4 == fde_len);
		}
		e_size = e.len;
		tail = 32;                 /* zeroed already: nops for the CIE program */

		/* Point the FDE at the fake image, and neutralise the two FDE fields
		 * whose bytes the CIE program will walk over. */
		memcpy (e.buf + f + 4, &(guint32){ (guint32) cie_ptr }, 4);
		memcpy (e.buf + f + 8, &(guint32){ 0 }, 4);   /* initial_location = 0 */
		memcpy (e.buf + f + 12, &(guint32){ 0 }, 4);  /* address_range = 0 */
		e.code_start = e.buf + f + 8;                 /* pcrel from a zero */

		/* The fake CIE image. */
		cie_len = (e_size + tail) - (t + 4);
		memcpy (e.buf + t, &(guint32){ (guint32) cie_len }, 4);
		memcpy (e.buf + t + 4, &(guint32){ 0 }, 4);   /* CIE id */
		e.buf [t + 8]  = 1;                           /* version */
		e.buf [t + 9]  = 'z';
		e.buf [t + 10] = 'R';
		e.buf [t + 11] = 0;
		e.buf [t + 12] = 1;                           /* code_align */
		e.buf [t + 13] = (guint8)(DATA_ALIGN & 0x7f); /* data_align, sleb -8 */
		e.buf [t + 14] = (guint8) PC_REG;             /* return column */
		e.buf [t + 15] = 1;                           /* augmentation length */
		e.buf [t + 16] = PE_pcrel | PE_sdata4;
		e.buf [t + 17] = DW_CFA_def_cfa;
		e.buf [t + 18] = 7;
		e.buf [t + 19] = 8;
		e.buf [t + 20] = (guint8)(DW_CFA_offset | PC_REG);
		e.buf [t + 21] = 1;

		g_assert (e_size + tail < (int) sizeof (e.buf));
		g_assert (cie_len > e_size - (t + 4));   /* it really does overrun */
		e.len = e_size;
		expect_decline ("fde-cie-pointer-to-length-past-section-declines", &e, 0x1000);
	}

	/*
	 * A CIE header truncated by the end of the section: the length field is
	 * there and consistent, but the id field that follows it is not.
	 */
	{
		guint32 cie_len;
		int fde_start, tail;

		memset (&e, 0, sizeof (e));
		std_cie (&e);
		build_fde (&e, 16, p.b, p.n);
		tail = e.len;
		b_u32 (&e, 1);           /* a CIE claiming one byte of contents */
		b_u8 (&e, 0);            /* ...which is all that is left */
		memcpy (&cie_len, e.buf, 4);
		fde_start = 4 + cie_len;
		memcpy (e.buf + fde_start + 4, &(guint32){ (guint32)(fde_start + 4 - tail) }, 4);
		expect_decline ("fde-cie-header-truncated-declines", &e, 16);
	}

	/* A CIE truncated immediately after its version byte: the augmentation
	 * string has no terminator inside the entry. */
	{
		memset (&e, 0, sizeof (e));
		b_u32 (&e, 5);           /* length: id + version */
		b_u32 (&e, 0);           /* CIE id */
		b_u8 (&e, 1);            /* version, and nothing after it */
		e.cie_off = 0;
		e.cie_has_z = FALSE;
		e.fde_ptr_enc = PE_pcrel | PE_sdata4;
		build_fde (&e, 16, p.b, p.n);
		expect_decline ("cie-truncated-after-version-declines", &e, 16);
	}
	/* A CIE truncated after the augmentation string: the alignments and the
	 * return column are not there. */
	{
		memset (&e, 0, sizeof (e));
		b_u32 (&e, 8);           /* length: id + version + "zR\0" */
		b_u32 (&e, 0);
		b_u8 (&e, 1);
		b_u8 (&e, 'z'); b_u8 (&e, 'R'); b_u8 (&e, 0);
		e.cie_off = 0;
		e.cie_has_z = TRUE;
		e.fde_ptr_enc = PE_pcrel | PE_sdata4;
		build_fde (&e, 16, p.b, p.n);
		expect_decline ("cie-truncated-before-alignments-declines", &e, 16);
	}
	/* A CIE truncated after the augmentation LENGTH: the 'R' byte the
	 * augmentation string promises is past the end of the entry. */
	{
		memset (&e, 0, sizeof (e));
		b_u32 (&e, 12);          /* id + version + "zR\0" + aligns + column + auglen */
		b_u32 (&e, 0);
		b_u8 (&e, 1);
		b_u8 (&e, 'z'); b_u8 (&e, 'R'); b_u8 (&e, 0);
		b_uleb (&e, 1);                  /* code_align */
		b_sleb (&e, DATA_ALIGN);         /* data_align */
		b_u8 (&e, (guint8) PC_REG);      /* return column */
		b_uleb (&e, 0);                  /* augmentation length: no data at all */
		e.cie_off = 0;
		e.cie_has_z = TRUE;
		e.fde_ptr_enc = PE_pcrel | PE_sdata4;
		build_fde (&e, 16, p.b, p.n);
		expect_decline ("cie-truncated-before-augmentation-data-declines", &e, 16);
	}
	/* An FDE whose absolute (8-byte) initial_location is cut in half. */
	{
		CieSpec s;

		memset (&e, 0, sizeof (e));
		cie_spec_init (&s);
		s.fde_ptr_enc = PE_absptr;
		build_cie_spec (&e, &s, cie.b, cie.n);
		b_u32 (&e, 8);                       /* contents: cie ptr + half a pointer */
		b_u32 (&e, (guint32)(e.len - e.cie_off));
		e.pcbegin_off = e.len;
		b_u32 (&e, 0x100);
		e.code_start = e.buf + 0x100;
		expect_decline ("fde-truncated-inside-absolute-pointer-declines", &e, 16);
	}

	/* An FDE truncated inside, and immediately after, its initial_location. */
	{
		int len_off;

		memset (&e, 0, sizeof (e));
		std_cie (&e);
		len_off = e.len;
		b_u32 (&e, 8);                       /* contents: cie ptr + pcbegin */
		b_u32 (&e, (guint32)(e.len - e.cie_off));
		e.pcbegin_off = e.len;
		b_u32 (&e, 0x100);
		e.code_start = e.buf + e.pcbegin_off + 0x100;
		(void) len_off;
		expect_decline ("fde-truncated-after-initial-location-declines", &e, 16);
	}
	{
		memset (&e, 0, sizeof (e));
		std_cie (&e);
		b_u32 (&e, 6);                       /* contents: cie ptr + half a pcbegin */
		b_u32 (&e, (guint32)(e.len - e.cie_off));
		e.pcbegin_off = e.len;
		b_u8 (&e, 0x00); b_u8 (&e, 0x01);
		e.code_start = e.buf + e.pcbegin_off + 0x100;
		expect_decline ("fde-truncated-inside-initial-location-declines", &e, 16);
	}
	/*
	 * An FDE augmentation length running past the end of the entry. As with the
	 * CIE case above, this pins the outcome and not the check: transcode_fde()'s
	 * "aug_len > r.remaining ()" can be deleted and the input still declines,
	 * through r.seek()'s own range check.
	 */
	{
		int len_off, start;

		memset (&e, 0, sizeof (e));
		std_cie (&e);
		len_off = e.len;
		b_u32 (&e, 0);
		start = e.len;
		b_u32 (&e, (guint32)(e.len - e.cie_off));
		e.pcbegin_off = e.len;
		b_u32 (&e, 0x100);
		e.code_start = e.buf + e.pcbegin_off + 0x100;
		b_u32 (&e, 16);          /* address_range */
		b_uleb (&e, 200);        /* augmentation length: far past the entry */
		b_u8 (&e, DW_CFA_nop);
		while ((e.len - start) % 4)
			b_u8 (&e, DW_CFA_nop);
		memcpy (e.buf + len_off, &(guint32){ e.len - start }, 4);
		expect_decline ("fde-augmentation-length-past-entry-end-declines", &e, 16);
	}
}

/* ------------------------------------------------------- the original cases */

/* DW_CFA_restore reverting a register to a rule the CIE DID establish. */
static void
test_restore_to_cie_rule (void)
{
	EhFrame e;
	GSList *ops = NULL;
	guint8 insns [16];
	int n = 0;
	/* pc is saved elsewhere, then restored to the CIE's cfa-8 */
	ExpOp exp [] = {
		{ 0, DW_CFA_def_cfa,   7,      8 },
		{ 0, DW_CFA_offset,    PC_REG, -8 },
		{ 2, DW_CFA_offset,    PC_REG, -16 },
		{ 4, DW_CFA_offset,    PC_REG, -8 },
	};

	memset (&e, 0, sizeof (e));
	std_cie (&e);
	insns [n++] = DW_CFA_advance_loc | 2;
	insns [n++] = (guint8)(DW_CFA_offset | PC_REG); insns [n++] = 2;  /* cfa-16 */
	insns [n++] = DW_CFA_advance_loc | 2;
	insns [n++] = (guint8)(DW_CFA_restore | PC_REG);
	build_fde (&e, 16, insns, n);

	expect_ops ("restore-to-cie-rule", &e, 16, exp, G_N_ELEMENTS (exp));
	(void) ops;
}

/*
 * DW_CFA_restore for a register the CIE has NO rule for. It must become
 * same_value: mono has no "unset" rule, and leaving the earlier offset in place
 * would keep restoring from a slot the epilogue has already popped.
 */
static void
test_restore_to_no_cie_rule (void)
{
	EhFrame e;
	guint8 insns [16];
	int n = 0;
	ExpOp exp [] = {
		{ 0, DW_CFA_def_cfa,    7,      8 },
		{ 0, DW_CFA_offset,     PC_REG, -8 },
		{ 2, DW_CFA_offset,     3,      -16 },
		{ 4, DW_CFA_same_value, 3,      0 },
	};

	memset (&e, 0, sizeof (e));
	std_cie (&e);
	insns [n++] = DW_CFA_advance_loc | 2;
	insns [n++] = (guint8)(DW_CFA_offset | 3); insns [n++] = 2;   /* rbx at cfa-16 */
	insns [n++] = DW_CFA_advance_loc | 2;
	insns [n++] = (guint8)(DW_CFA_restore | 3);
	build_fde (&e, 16, insns, n);

	expect_ops ("restore-to-no-cie-rule", &e, 16, exp, G_N_ELEMENTS (exp));
}

/*
 * DW_CFA_undefined on the return column. The CIE's "pc at cfa-8" is restated at
 * offset 0, so this MUST emit something that cancels it; otherwise
 * mono_unwind_frame keeps loading a return address from a slot that no longer
 * holds one. (This is the case that regressed once already.)
 */
static void
test_undefined_cancels_cie_rule (void)
{
	EhFrame e;
	guint8 insns [16];
	int n = 0;
	ExpOp exp [] = {
		{ 0, DW_CFA_def_cfa,    7,      8 },
		{ 0, DW_CFA_offset,     PC_REG, -8 },
		{ 2, DW_CFA_same_value, PC_REG, 0 },
	};

	memset (&e, 0, sizeof (e));
	std_cie (&e);
	insns [n++] = DW_CFA_advance_loc | 2;
	insns [n++] = DW_CFA_undefined; insns [n++] = (guint8) PC_REG;
	build_fde (&e, 16, insns, n);

	expect_ops ("undefined-cancels-cie-rule", &e, 16, exp, G_N_ELEMENTS (exp));
}

/*
 * Nested remember_state/restore_state. mono's own unwinder supports only one
 * level (UnwindState state_stack[1]) and hard-fails at depth 2, so the
 * transcoder must simulate the stack and materialize the restored rules,
 * emitting neither opcode.
 */
static void
test_nested_remember_restore (void)
{
	EhFrame e;
	GSList *ops = NULL;
	guint8 insns [32];
	int n = 0;
	ExpOp exp [] = {
		{ 0, DW_CFA_def_cfa,       7,  8 },
		{ 0, DW_CFA_offset,        PC_REG, -8 },
		{ 1, DW_CFA_def_cfa_offset, -1, 16 },
		{ 2, DW_CFA_def_cfa_offset, -1, 24 },
		{ 3, DW_CFA_def_cfa_offset, -1, 32 },
		/* restore_state (inner) -> back to 24 */
		{ 4, DW_CFA_def_cfa,       7,  24 },
		/* restore_state (outer) -> back to 16 */
		{ 5, DW_CFA_def_cfa,       7,  16 },
	};

	memset (&e, 0, sizeof (e));
	std_cie (&e);
	insns [n++] = DW_CFA_advance_loc | 1;
	insns [n++] = DW_CFA_def_cfa_offset; insns [n++] = 16;
	insns [n++] = DW_CFA_remember_state;              /* depth 1 (cfa 16) */
	insns [n++] = DW_CFA_advance_loc | 1;
	insns [n++] = DW_CFA_def_cfa_offset; insns [n++] = 24;
	insns [n++] = DW_CFA_remember_state;              /* depth 2 (cfa 24) */
	insns [n++] = DW_CFA_advance_loc | 1;
	insns [n++] = DW_CFA_def_cfa_offset; insns [n++] = 32;
	insns [n++] = DW_CFA_advance_loc | 1;
	insns [n++] = DW_CFA_restore_state;               /* -> 24 */
	insns [n++] = DW_CFA_advance_loc | 1;
	insns [n++] = DW_CFA_restore_state;               /* -> 16 */
	build_fde (&e, 16, insns, n);

	current_case = "nested-remember-restore";
	if (!transcode (&e, 16, &ops)) {
		printf ("FAIL nested-remember-restore: declined\n"); failures ++; cases_run ++; return;
	}
	check_ops ("nested-remember-restore", ops, exp, G_N_ELEMENTS (exp));

	/* Neither opcode may survive into the op list. */
	for (GSList *l = ops; l; l = l->next) {
		MonoUnwindOp *o = (MonoUnwindOp*) l->data;
		if (o->op == DW_CFA_remember_state || o->op == DW_CFA_restore_state) {
			printf ("FAIL nested-remember-restore: emitted remember/restore_state\n");
			failures ++;
			break;
		}
	}
	mono_free_unwind_info (ops);
}

/* ------------------------------------------------------ the random generator */

/*
 * A bounded, fixed-seed INVARIANT test. It checks no golden bytes - the
 * expected output of a random CFI program is not a thing worth writing down,
 * and a table of it would only pin today's behaviour. What it asserts instead:
 *
 *   - the transcoder never reads outside the section. Every iteration is run
 *     from a private mapping whose section ends FLUSH against a PROT_NONE guard
 *     page, so a single byte read past the end is a SIGSEGV in the ordinary
 *     build - no sanitizer required. (The previous version used an exactly
 *     sized g_malloc and a comment about ASAN; nothing in this build runs ASAN
 *     or valgrind, so it detected nothing.)
 *   - ACCEPTED IFF the documented preconditions hold. Programs built only from
 *     representable opcodes with in-range operands MUST be accepted (MODE_OK);
 *     the same programs under a CIE whose INITIAL INSTRUCTIONS the transcoder
 *     cannot represent MUST be declined (MODE_BAD_CIE).
 *   - whatever is accepted is WELL-FORMED: it starts with a def_cfa at offset
 *     0, contains only the five opcodes mono_unwind_frame() can execute, its
 *     code offsets are non-decreasing and within the function, its register
 *     offsets are non-positive and its CFA offsets non-negative, it is no
 *     longer than MAX_UNWIND_OPS, and mono_unwind_ops_encode() can encode it.
 *   - the result is ADDRESS-INDEPENDENT: every section is transcoded twice,
 *     from two mappings at different addresses, and the op lists must be
 *     identical. FDE pointers are pc-relative, so this is a property that can
 *     actually break; running the same buffer twice, as this used to, is a pure
 *     function called twice and cannot.
 *
 * ---- what the end-of-run assertions are, and why they are not decoration ----
 *
 * The first version of this asserted only "at least one acceptance and at least
 * one decline overall". Modes 0 and 1 guarantee one of each BY CONSTRUCTION, so
 * that assertion could not fail - the exact defect (a check that cannot fail)
 * this file exists to avoid, reintroduced in the test that was supposed to
 * replace it. The counters are per mode now, and the assertions are the ones
 * that can be violated:
 *
 *   MODE_OK        every iteration must be accepted (0 declines).
 *   MODE_BAD_CIE   every iteration must be declined (0 accepts).
 *   MODE_MUTATED   BOTH outcomes must occur. This is the real one: it fails if
 *                  the mutations become too weak (everything still accepted) or
 *                  too destructive (nothing survives), and it is what makes
 *                  fuzz_check_ops() run against inputs the generator did not
 *                  itself construct. The old MODE 2 produced ONE acceptance in
 *                  750 iterations, so the invariant checker was, in practice,
 *                  only ever fed the generator's own output.
 *   MODE_RANDOM    asserted only not to crash or violate the invariants; a
 *                  uniformly random buffer is essentially never a valid
 *                  section, and pretending otherwise would be another
 *                  assertion that cannot fail.
 */

static guint32 rng_state;

static guint32
rnd (void)
{
	/* xorshift32: fixed, deterministic, and not the C library's. */
	guint32 x = rng_state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

static guint32
rnd_below (guint32 n)
{
	return rnd () % n;
}

static int fuzz_accepted;
static int fuzz_declined;

/* Registers the generator uses: the callee-saved set plus the return column. */
static const int fuzz_regs [] = { 3, 6, 12, 13, 14, 15 };

#define FUZZ_CODE_LEN 0x10000

/*
 * Emit one representable opcode with in-range operands. DEPTH tracks the
 * remember_state stack so restore_state is only emitted when it can succeed.
 */
static void
fuzz_emit_wellformed_op (Insn *p, int *depth, guint32 *loc)
{
	int reg = fuzz_regs [rnd_below (G_N_ELEMENTS (fuzz_regs))];

	switch (rnd_below (12)) {
	case 0:
		i_u8 (p, DW_CFA_nop);
		break;
	case 1:
		/* Bounded so the total stays well inside FUZZ_CODE_LEN. */
		if (*loc + 63 < FUZZ_CODE_LEN) {
			guint32 d = 1 + rnd_below (63);
			i_u8 (p, (guint8)(DW_CFA_advance_loc | d));
			*loc += d;
		} else {
			i_u8 (p, DW_CFA_nop);
		}
		break;
	case 2:
		i_u8 (p, (guint8)(DW_CFA_offset | reg));
		i_uleb (p, 1 + rnd_below (7));   /* the range real output uses */
		break;
	case 3:
		i_u8 (p, DW_CFA_offset_extended);
		i_uleb (p, reg);
		i_uleb (p, 1 + rnd_below (7));
		break;
	case 4:
		i_u8 (p, DW_CFA_offset_extended_sf);
		i_uleb (p, reg);
		i_sleb (p, 1 + (gint64) rnd_below (7));
		break;
	case 5:
		i_u8 (p, DW_CFA_def_cfa);
		i_uleb (p, 7);
		i_uleb (p, 8 + rnd_below (64) * 8);
		break;
	case 6:
		i_u8 (p, DW_CFA_def_cfa_register);
		i_uleb (p, (rnd () & 1) ? 6 : 7);
		break;
	case 7:
		i_u8 (p, DW_CFA_def_cfa_offset);
		i_uleb (p, 8 + rnd_below (64) * 8);
		break;
	case 8:
		i_u8 (p, DW_CFA_same_value);
		i_uleb (p, reg);
		break;
	case 9:
		if (*depth < 8) {
			i_u8 (p, DW_CFA_remember_state);
			(*depth) ++;
		} else {
			i_u8 (p, DW_CFA_nop);
		}
		break;
	case 10:
		if (*depth > 0) {
			i_u8 (p, DW_CFA_restore_state);
			(*depth) --;
		} else {
			i_u8 (p, DW_CFA_nop);
		}
		break;
	default:
		i_u8 (p, (guint8)(DW_CFA_restore | reg));
		break;
	}
}

/*
 * Check the invariants an accepted op list must satisfy. Returns FALSE (and
 * prints) on the first violation.
 */
static gboolean
fuzz_check_ops (const char *what, GSList *ops, guint32 code_len)
{
	GSList *l;
	int n = 0;
	guint32 last_when = 0;
	MonoUnwindOp *first;
	guint8 *encoded;
	guint32 encoded_len = 0;

	if (!ops) {
		printf ("FAIL %s: accepted but returned no ops\n", what);
		return FALSE;
	}
	first = (MonoUnwindOp*) ops->data;
	if (first->op != DW_CFA_def_cfa || first->when != 0) {
		printf ("FAIL %s: first op is %s at %u, want def_cfa at 0\n",
			what, op_name (first->op), first->when);
		return FALSE;
	}

	for (l = ops; l; l = l->next) {
		MonoUnwindOp *o = (MonoUnwindOp*) l->data;

		n ++;
		if (o->when < last_when) {
			printf ("FAIL %s: op %d goes backwards (%u after %u)\n",
				what, n, o->when, last_when);
			return FALSE;
		}
		last_when = o->when;
		if (o->when > code_len) {
			printf ("FAIL %s: op %d at %u past code_len %u\n", what, n, o->when, code_len);
			return FALSE;
		}
		switch (o->op) {
		case DW_CFA_def_cfa:
		case DW_CFA_def_cfa_offset:
			if (o->val < 0) {
				printf ("FAIL %s: op %d: negative CFA offset %d\n", what, n, o->val);
				return FALSE;
			}
			break;
		case DW_CFA_offset:
			if (o->val > 0) {
				printf ("FAIL %s: op %d: register saved above the CFA (%d)\n",
					what, n, o->val);
				return FALSE;
			}
			if (o->val % DATA_ALIGN != 0) {
				printf ("FAIL %s: op %d: offset %d does not re-factor exactly\n",
					what, n, o->val);
				return FALSE;
			}
			break;
		case DW_CFA_def_cfa_register:
		case DW_CFA_same_value:
			break;
		default:
			printf ("FAIL %s: op %d: %s (0x%x) is not executable by mono_unwind_frame\n",
				what, n, op_name (o->op), o->op);
			return FALSE;
		}
		if (o->op != DW_CFA_def_cfa_offset) {
			int dreg = mono_hw_reg_to_dwarf_reg (o->reg);
			if (dreg < 0 || dreg > PC_REG) {
				printf ("FAIL %s: op %d: dwarf register %d out of range\n", what, n, dreg);
				return FALSE;
			}
		}
	}

	if (n > EH_MAX_UNWIND_OPS) {
		printf ("FAIL %s: %d ops, over the %d cap\n", what, n, EH_MAX_UNWIND_OPS);
		return FALSE;
	}

	/* mono must be able to encode what it was handed. */
	encoded = mono_unwind_ops_encode (ops, &encoded_len);
	if (!encoded || encoded_len == 0 || encoded_len >= 4096) {
		printf ("FAIL %s: encode produced %u bytes\n", what, encoded_len);
		g_free (encoded);
		return FALSE;
	}
	g_free (encoded);
	return TRUE;
}

static gboolean
fuzz_lists_equal (GSList *a, GSList *b)
{
	for (; a && b; a = a->next, b = b->next) {
		MonoUnwindOp *x = (MonoUnwindOp*) a->data;
		MonoUnwindOp *y = (MonoUnwindOp*) b->data;

		if (x->op != y->op || x->reg != y->reg || x->val != y->val || x->when != y->when)
			return FALSE;
	}
	return a == NULL && b == NULL;
}

/*
 * A private mapping whose last page is PROT_NONE and whose section ends flush
 * against it. Any read past the end of the section faults; there is no slack
 * for an overrun to land in quietly. Falls back to a plain allocation where
 * mmap is unavailable, in which case the guard is simply absent (this file only
 * builds on !HOST_WIN32 today, so the fallback is not exercised here).
 */
typedef struct {
	guint8 *map;      /* base of the mapping, or the malloc block */
	gsize map_len;
	guint8 *section;  /* the section itself: [section, section + len) */
	gboolean mapped;
} GuardBuf;

#if defined (HAVE_SYS_MMAN_H) && defined (HAVE_UNISTD_H) && !defined (HOST_WIN32)
#define EHFRAME_HAVE_GUARD_PAGE 1
#endif

static gboolean
guard_alloc (GuardBuf *g, const guint8 *bytes, int len)
{
#ifdef EHFRAME_HAVE_GUARD_PAGE
	gsize page = (gsize) sysconf (_SC_PAGESIZE);
	gsize data = ((gsize) len + page - 1) / page * page;

	if (data == 0)
		data = page;
	g->map_len = data + page;
	g->map = (guint8*) mmap (NULL, g->map_len, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (g->map == MAP_FAILED) {
		g->map = NULL;
		return FALSE;
	}
	/* The trailing page faults on any access. */
	if (mprotect (g->map + data, page, PROT_NONE) != 0) {
		munmap (g->map, g->map_len);
		g->map = NULL;
		return FALSE;
	}
	g->mapped = TRUE;
	/* Flush against the guard: section_end == the first unreadable byte. */
	g->section = g->map + data - len;
	memcpy (g->section, bytes, len);
	return TRUE;
#else
	g->mapped = FALSE;
	g->map_len = len ? len : 1;
	g->map = (guint8*) g_malloc (g->map_len);
	g->section = g->map;
	memcpy (g->section, bytes, len);
	return TRUE;
#endif
}

static void
guard_free (GuardBuf *g)
{
	if (!g->map)
		return;
#ifdef EHFRAME_HAVE_GUARD_PAGE
	if (g->mapped)
		munmap (g->map, g->map_len);
	else
#endif
		g_free (g->map);
	g->map = NULL;
}

static int fuzz_guard_pages;   /* how many iterations really got a guard page */

/*
 * Run one generated section, twice, from two mappings at DIFFERENT addresses.
 * Returns FALSE on an invariant violation. MUST_ACCEPT/MUST_DECLINE express the
 * preconditions the caller built in.
 *
 * CODE_OFF is the described code address as an offset INTO the section: FDE
 * pointers are pc-relative, so it has to be re-derived at whichever address the
 * section landed - which is also what makes the second run a real check rather
 * than the same pure call twice.
 */
static gboolean
fuzz_run (const char *what, const guint8 *bytes, int len, int code_off,
          guint32 code_len, gboolean must_accept, gboolean must_decline)
{
	GuardBuf a, b;
	GSList *ops = NULL, *ops2 = NULL;
	gboolean r, r2, good = TRUE;

	current_case = what;
	memset (&a, 0, sizeof (a));
	memset (&b, 0, sizeof (b));
	if (!guard_alloc (&a, bytes, len) || !guard_alloc (&b, bytes, len)) {
		printf ("FAIL %s: could not map a guarded buffer\n", what);
		guard_free (&a);
		guard_free (&b);
		return FALSE;
	}
	if (a.mapped)
		fuzz_guard_pages ++;

	r = mono_llvm_eh_frame_to_unwind_ops (a.section, (guint32) len,
					      a.section + code_off, code_len, &ops);

	if (r)
		fuzz_accepted ++;
	else
		fuzz_declined ++;

	if (r && must_decline) {
		printf ("FAIL %s: accepted an input that violates the preconditions\n", what);
		good = FALSE;
	}
	if (!r && must_accept) {
		printf ("FAIL %s: declined a well-formed input\n", what);
		good = FALSE;
	}
	if (!r && ops) {
		printf ("FAIL %s: declined but returned ops\n", what);
		good = FALSE;
	}
	if (r && good)
		good = fuzz_check_ops (what, ops, code_len);

	/* Same bytes, different address: the answer must not move with it. */
	r2 = mono_llvm_eh_frame_to_unwind_ops (b.section, (guint32) len,
					       b.section + code_off, code_len, &ops2);
	if (r2 != r || !fuzz_lists_equal (ops, ops2)) {
		printf ("FAIL %s: result depends on the section's address\n", what);
		good = FALSE;
	}

	mono_free_unwind_info (ops);
	mono_free_unwind_info (ops2);
	guard_free (&a);
	guard_free (&b);
	return good;
}

/*
 * Give the CIE's INITIAL INSTRUCTIONS something the transcoder must refuse.
 *
 * Every one of these survives CIE header validation - version, alignments and
 * return column are untouched - so unlike the header poisons this replaced, the
 * CIE interpreter actually runs, on the generated program, and the decline
 * comes from it (or, for the last one, from the missing CFA rule afterwards).
 */
static void
fuzz_poison_cie_program (Insn *cie, guint32 which)
{
	switch (which % 5) {
	case 0:
		/* Absolute location: not representable. */
		i_u8 (cie, DW_CFA_set_loc);
		i_u8 (cie, 0); i_u8 (cie, 0); i_u8 (cie, 0); i_u8 (cie, 0);
		i_u8 (cie, 0); i_u8 (cie, 0); i_u8 (cie, 0); i_u8 (cie, 0);
		break;
	case 1:
		/* An expression would need an evaluator in async-signal context. */
		i_u8 (cie, DW_CFA_def_cfa_expression); i_uleb (cie, 1); i_u8 (cie, 0x9c);
		break;
	case 2:
		/* DW_CFA_restore inside the CIE has no initial rules to revert to. */
		i_u8 (cie, (guint8)(DW_CFA_restore | 3));
		break;
	case 3:
		/* The CIE's own program running past the end of the function. */
		i_u8 (cie, DW_CFA_advance_loc4);
		i_u8 (cie, 0xff); i_u8 (cie, 0xff); i_u8 (cie, 0xff); i_u8 (cie, 0x7f);
		break;
	default:
		/* No CFA rule at all: nothing to unwind with. Rebuild without it. */
		cie->n = 0;
		i_u8 (cie, (guint8)(DW_CFA_offset | PC_REG)); i_uleb (cie, 1);
		break;
	}
}

/*
 * Replace one byte of a well-formed program with a random one. The result is
 * neither well-formed nor random: it is mostly-valid CFI with one thing wrong,
 * which is the shape that actually produces a mix of verdicts - and therefore
 * the shape that gets fuzz_check_ops() run against something the generator did
 * not construct.
 */
static void
fuzz_mutate (Insn *p)
{
	if (p->n <= 0)
		return;
	p->b [rnd_below ((guint32) p->n)] = (guint8) rnd ();
}

enum {
	MODE_OK = 0,        /* well-formed          -> must be accepted */
	MODE_BAD_CIE,       /* unrepresentable CIE  -> must be declined */
	MODE_MUTATED,       /* one byte corrupted   -> both outcomes required */
	MODE_RANDOM,        /* uniform noise        -> invariants only */
	MODE_COUNT
};

static const char *fuzz_mode_name [MODE_COUNT] = {
	"fuzz-wellformed", "fuzz-unrepresentable-cie", "fuzz-mutated", "fuzz-random-section"
};

static void
test_random_programs (void)
{
	const int iterations = 3000;
	int accepted [MODE_COUNT], declined [MODE_COUNT];
	int i, m;
	int reported = 0;
	int before_a, before_d;

	rng_state = FUZZ_SEED;
	fuzz_accepted = 0;
	fuzz_declined = 0;
	fuzz_guard_pages = 0;
	memset (accepted, 0, sizeof (accepted));
	memset (declined, 0, sizeof (declined));
	cases_run ++;

	for (i = 0; i < iterations; ++i) {
		EhFrame e;
		Insn p;
		CieSpec s;
		Insn cie;
		int depth = 0;
		guint32 loc = 0;
		int nops, j;
		gboolean ok;
		int mode = i % MODE_COUNT;

		current_iteration = i;
		memset (&e, 0, sizeof (e));
		cie_spec_init (&s);
		std_cie_insns (&cie);
		p.n = 0;
		before_a = fuzz_accepted;
		before_d = fuzz_declined;

		/*
		 * A restore_state can materialize up to one CFA rule plus one rule per
		 * register, so 12 opcodes cannot overrun MAX_UNWIND_OPS.
		 */
		nops = 1 + (int) rnd_below (12);

		switch (mode) {
		case MODE_OK:
			for (j = 0; j < nops; ++j)
				fuzz_emit_wellformed_op (&p, &depth, &loc);
			build_cie_spec (&e, &s, cie.b, cie.n);
			build_fde (&e, FUZZ_CODE_LEN, p.b, p.n);
			ok = fuzz_run (fuzz_mode_name [mode], e.buf, e.len,
			               (int)((guint8*) code_start_of (&e) - e.buf),
			               FUZZ_CODE_LEN, TRUE, FALSE);
			break;
		case MODE_BAD_CIE:
			for (j = 0; j < nops; ++j)
				fuzz_emit_wellformed_op (&p, &depth, &loc);
			fuzz_poison_cie_program (&cie, rnd ());
			build_cie_spec (&e, &s, cie.b, cie.n);
			build_fde (&e, FUZZ_CODE_LEN, p.b, p.n);
			ok = fuzz_run (fuzz_mode_name [mode], e.buf, e.len,
			               (int)((guint8*) code_start_of (&e) - e.buf),
			               FUZZ_CODE_LEN, FALSE, TRUE);
			break;
		case MODE_MUTATED:
			for (j = 0; j < nops; ++j)
				fuzz_emit_wellformed_op (&p, &depth, &loc);
			fuzz_mutate (&p);
			build_cie_spec (&e, &s, cie.b, cie.n);
			build_fde (&e, FUZZ_CODE_LEN, p.b, p.n);
			ok = fuzz_run (fuzz_mode_name [mode], e.buf, e.len,
			               (int)((guint8*) code_start_of (&e) - e.buf),
			               FUZZ_CODE_LEN, FALSE, FALSE);
			break;
		default: { /* an entirely random section */
			int len = 4 + (int) rnd_below (120);

			for (j = 0; j < len; ++j)
				e.buf [j] = (guint8) rnd ();
			e.len = len;
			ok = fuzz_run (fuzz_mode_name [mode], e.buf, e.len, 0x100,
			               FUZZ_CODE_LEN, FALSE, FALSE);
			break;
		}
		}

		accepted [mode] += fuzz_accepted - before_a;
		declined [mode] += fuzz_declined - before_d;

		if (!ok && reported < 5) {
			printf ("     (iteration %d, mode %s)\n", i, fuzz_mode_name [mode]);
			reported ++;
			failures ++;
		}
	}

	/*
	 * Per-mode contracts. See the block comment above for why the old
	 * "some acceptance and some decline happened overall" check was worthless.
	 */
	if (declined [MODE_OK] != 0) {
		printf ("FAIL random-programs: %d well-formed inputs declined\n", declined [MODE_OK]);
		failures ++;
	}
	if (accepted [MODE_BAD_CIE] != 0) {
		printf ("FAIL random-programs: %d unrepresentable CIEs accepted\n",
			accepted [MODE_BAD_CIE]);
		failures ++;
	}
	if (accepted [MODE_MUTATED] == 0 || declined [MODE_MUTATED] == 0) {
		printf ("FAIL random-programs: mutated mode is one-sided (%d accepted, %d declined) - "
			"the invariant checks are not being fed anything the generator did not build\n",
			accepted [MODE_MUTATED], declined [MODE_MUTATED]);
		failures ++;
	}
#ifdef EHFRAME_HAVE_GUARD_PAGE
	if (fuzz_guard_pages != iterations) {
		printf ("FAIL random-programs: only %d of %d iterations were guard-paged\n",
			fuzz_guard_pages, iterations);
		failures ++;
	}
#endif

	current_iteration = -1;
	printf ("ok   random-programs (%d iterations", iterations);
	for (m = 0; m < MODE_COUNT; ++m)
		printf (", %s %d/%d", fuzz_mode_name [m] + 5, accepted [m], declined [m]);
	printf (" accepted/declined)\n");
}

/* ---------------------------------------------------------------- driver */

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_ehframe_main (void);

int
test_llvm_ehframe_main (void)
{
	failures = 0;
	cases_run = 0;

	/*
	 * Line buffering, so a fatal signal cannot take the progress with it: under
	 * `make check` stdout is a file, hence block buffered by default, and a
	 * crash would leave an empty log next to a bare "Segmentation fault". With
	 * this, the log ends at the last case that completed and the handler names
	 * the one that did not. 174 lines of output; the cost is irrelevant.
	 */
	setvbuf (stdout, NULL, _IOLBF, 0);
	install_crash_handler ();

	/*
	 * Every byte sequence below encodes x86-64 DWARF register numbers and
	 * offsets factored by -8. Running them anywhere else would test the
	 * builder, not the transcoder.
	 */
	if (mono_unwind_get_dwarf_data_align () != -8 || mono_unwind_get_dwarf_pc_reg () != 16) {
		printf ("skipped: not x86-64 DWARF (data_align=%d pc_reg=%d)\n",
			mono_unwind_get_dwarf_data_align (), mono_unwind_get_dwarf_pc_reg ());
		remove_crash_handler ();
		return 0;
	}

	cases_callee_saved_offsets ();
	cases_factored_bounds ();
	cases_advance_loc ();
	cases_def_cfa ();
	cases_remember_restore ();
	cases_restore ();
	cases_leb128 ();
	cases_op_limit ();
	cases_cie ();
	cases_fde_pointer_encodings ();
	cases_section_framing ();

	test_restore_to_cie_rule ();
	test_restore_to_no_cie_rule ();
	test_undefined_cancels_cie_rule ();
	test_nested_remember_restore ();

	test_random_programs ();

	remove_crash_handler ();
	current_case = "(finished)";

	if (failures) {
		printf ("%d failure(s) in %d cases\n", failures, cases_run);
		return 1;
	}
	printf ("all ok (%d cases)\n", cases_run);
	return 0;
}

#else /* !ENABLE_LLVM */

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_ehframe_main (void);

int
test_llvm_ehframe_main (void)
{
	return 0;
}

#endif
