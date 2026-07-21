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
 */

#include "config.h"

#include <stdio.h>
#include <string.h>
#include <glib.h>

#include "mini/mini.h"
#include "mini/mini-unwind.h"

#ifdef ENABLE_LLVM

/*
 * Declared here rather than by including llvm/backend.h: that header resolves
 * "mini.h" relative to its own directory, which does not work from here. Same
 * approach as test-llvm-engine.c.
 */
gboolean mono_llvm_eh_frame_to_unwind_ops (guint8 *eh_frame, guint32 eh_frame_size,
					   gpointer code_start, guint32 code_len,
					   GSList **out_ops);

/* ------------------------------------------------------------ builder */

typedef struct {
	guint8 buf [512];
	int len;
	/* Offset of the FDE's initial_location field, for deriving code_start. */
	int pcbegin_off;
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
 * Emit a CIE with augmentation "zR" (pcrel|sdata4 FDE pointers), code_align 1,
 * mono's data alignment, and mono's return column - i.e. exactly the shape LLVM
 * produces on this target. CIE_INSNS/CIE_LEN are the initial instructions.
 */
static void
build_cie (EhFrame *e, const guint8 *cie_insns, int cie_len)
{
	int len_off, start;

	len_off = e->len;
	b_u32 (e, 0);           /* length, patched below */
	start = e->len;
	b_u32 (e, 0);           /* CIE id */
	b_u8 (e, 1);            /* version */
	b_u8 (e, 'z'); b_u8 (e, 'R'); b_u8 (e, 0);
	b_uleb (e, 1);                                        /* code_align */
	b_sleb (e, mono_unwind_get_dwarf_data_align ());      /* data_align */
	b_u8 (e, (guint8) mono_unwind_get_dwarf_pc_reg ());   /* return column */
	b_uleb (e, 1);          /* augmentation length */
	b_u8 (e, 0x1b);         /* 'R': DW_EH_PE_pcrel | DW_EH_PE_sdata4 */
	if (cie_len)
		memcpy (e->buf + e->len, cie_insns, cie_len), e->len += cie_len;
	while ((e->len - start) % 4)
		b_u8 (e, DW_CFA_nop);
	memcpy (e->buf + len_off, &(guint32){ e->len - start }, 4);
}

/* Emit an FDE for the CIE at offset 0, with the given CFI program. */
static void
build_fde (EhFrame *e, guint32 code_len, const guint8 *insns, int insns_len)
{
	int len_off, start;

	len_off = e->len;
	b_u32 (e, 0);           /* length, patched below */
	start = e->len;
	b_u32 (e, (guint32)(e->len - 0)); /* distance back to the CIE at offset 0 */
	e->pcbegin_off = e->len;
	b_u32 (e, 0x100);       /* initial_location, pcrel; code_start derived from it */
	b_u32 (e, code_len);    /* address_range */
	b_uleb (e, 0);          /* augmentation length */
	if (insns_len)
		memcpy (e->buf + e->len, insns, insns_len), e->len += insns_len;
	while ((e->len - start) % 4)
		b_u8 (e, DW_CFA_nop);
	memcpy (e->buf + len_off, &(guint32){ e->len - start }, 4);
}

/* The code address the built FDE describes (pcrel from initial_location). */
static gpointer
code_start_of (EhFrame *e)
{
	return e->buf + e->pcbegin_off + 0x100;
}

/* ------------------------------------------------------------ checking */

static int failures;

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

/* Expected op, in DWARF register numbering. */
typedef struct { guint32 when; int op; int dwarf_reg; int val; } ExpOp;

static void
check_ops (const char *what, GSList *ops, const ExpOp *exp, int nexp)
{
	GSList *l = ops;
	int i = 0;

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

/* ------------------------------------------------------------ the tests */

/*
 * The CIE used throughout: CFA = r7 + 8, return column saved at cfa-8. That is
 * the standard x86-64 entry state and is what LLVM emits.
 */
#define DATA_ALIGN (mono_unwind_get_dwarf_data_align ())
#define PC_REG     (mono_unwind_get_dwarf_pc_reg ())

static void
std_cie (EhFrame *e)
{
	guint8 insns [16];
	int n = 0;

	insns [n++] = DW_CFA_def_cfa; insns [n++] = 7; insns [n++] = 8;
	/* DW_CFA_offset(pc) with factored offset 1 => 1 * -8 = cfa-8 */
	insns [n++] = (guint8)(DW_CFA_offset | PC_REG); insns [n++] = 1;
	build_cie (e, insns, n);
}

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

	if (!transcode (&e, 16, &ops)) {
		printf ("FAIL restore-to-cie-rule: declined\n"); failures ++; return;
	}
	check_ops ("restore-to-cie-rule", ops, exp, G_N_ELEMENTS (exp));
	mono_free_unwind_info (ops);
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
	GSList *ops = NULL;
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

	if (!transcode (&e, 16, &ops)) {
		printf ("FAIL restore-to-no-cie-rule: declined\n"); failures ++; return;
	}
	check_ops ("restore-to-no-cie-rule", ops, exp, G_N_ELEMENTS (exp));
	mono_free_unwind_info (ops);
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
	GSList *ops = NULL;
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

	if (!transcode (&e, 16, &ops)) {
		printf ("FAIL undefined-cancels-cie-rule: declined\n"); failures ++; return;
	}
	check_ops ("undefined-cancels-cie-rule", ops, exp, G_N_ELEMENTS (exp));
	mono_free_unwind_info (ops);
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

	if (!transcode (&e, 16, &ops)) {
		printf ("FAIL nested-remember-restore: declined\n"); failures ++; return;
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

/* An op list that would overrun mono_unwind_ops_encode_full()'s buf[4096]. */
static void
test_too_many_ops_declines (void)
{
	EhFrame e;
	guint8 insns [400];
	int n = 0;

	memset (&e, 0, sizeof (e));
	std_cie (&e);
	/* Alternate advance/def_cfa_offset to generate a long op list. */
	for (int i = 0; i < 130; ++i) {
		insns [n++] = DW_CFA_advance_loc | 1;
		insns [n++] = DW_CFA_def_cfa_offset;
		insns [n++] = (guint8)(16 + (i % 8) * 8);
	}
	build_fde (&e, 4096, insns, n);
	expect_decline ("too-many-ops", &e, 4096);
}

static void
test_malformed_declines (void)
{
	EhFrame e;
	guint8 insns [16];
	int n;

	/* Expression opcodes cannot be evaluated in async-signal context. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	n = 0;
	insns [n++] = DW_CFA_def_cfa_expression; insns [n++] = 1; insns [n++] = 0x9c;
	build_fde (&e, 16, insns, n);
	expect_decline ("expression-opcode", &e, 16);

	/* A register above what mono maps must decline, not be dropped silently. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	n = 0;
	insns [n++] = DW_CFA_offset_extended; insns [n++] = 60; insns [n++] = 2;
	build_fde (&e, 16, insns, n);
	expect_decline ("register-out-of-range", &e, 16);

	/* CFI advancing past the end of the function. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	n = 0;
	insns [n++] = DW_CFA_advance_loc4;
	insns [n++] = 0xff; insns [n++] = 0xff; insns [n++] = 0xff; insns [n++] = 0x7f;
	build_fde (&e, 16, insns, n);
	expect_decline ("advance-past-code-end", &e, 16);

	/* An operand truncated by the end of the entry. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	n = 0;
	insns [n++] = DW_CFA_def_cfa; insns [n++] = 7; /* offset operand missing */
	build_fde (&e, 16, insns, n);
	/* The padding nops make this well-formed-but-odd rather than truncated,
	 * so instead truncate the whole section below. */

	/* Truncated section: keep only part of the FDE. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	n = 0;
	insns [n++] = DW_CFA_advance_loc | 1;
	insns [n++] = DW_CFA_def_cfa_offset; insns [n++] = 16;
	build_fde (&e, 16, insns, n);
	{
		GSList *ops = NULL;
		gpointer cs = code_start_of (&e);
		/* Chop the buffer mid-FDE; the length field now overruns the section. */
		if (mono_llvm_eh_frame_to_unwind_ops (e.buf, (guint32)(e.len - 6), cs, 16, &ops)) {
			printf ("FAIL truncated-section: expected decline\n");
			failures ++;
			mono_free_unwind_info (ops);
		} else {
			printf ("ok   truncated-section (declined)\n");
		}
	}

	/* An FDE whose CIE pointer points outside the section. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	n = 0;
	insns [n++] = DW_CFA_nop;
	build_fde (&e, 16, insns, n);
	{
		GSList *ops = NULL;
		gpointer cs = code_start_of (&e);
		int fde_start = 0;
		/* Locate the FDE (first entry after the CIE) and corrupt its CIE ptr. */
		guint32 cie_len;
		memcpy (&cie_len, e.buf, 4);
		fde_start = 4 + cie_len;
		memcpy (e.buf + fde_start + 4, &(guint32){ 0x40000000 }, 4);
		if (mono_llvm_eh_frame_to_unwind_ops (e.buf, (guint32) e.len, cs, 16, &ops)) {
			printf ("FAIL bogus-cie-pointer: expected decline\n");
			failures ++;
			mono_free_unwind_info (ops);
		} else {
			printf ("ok   bogus-cie-pointer (declined)\n");
		}
	}

	/* No FDE matches the requested code address. */
	memset (&e, 0, sizeof (e));
	std_cie (&e);
	n = 0;
	insns [n++] = DW_CFA_nop;
	build_fde (&e, 16, insns, n);
	{
		GSList *ops = NULL;
		if (mono_llvm_eh_frame_to_unwind_ops (e.buf, (guint32) e.len,
						      (gpointer)(e.buf + 0x999), 16, &ops)) {
			printf ("FAIL no-matching-fde: expected decline\n");
			failures ++;
			mono_free_unwind_info (ops);
		} else {
			printf ("ok   no-matching-fde (declined)\n");
		}
	}
}

/* A CIE that establishes no CFA rule must decline rather than yield a list. */
static void
test_no_cfa_rule_declines (void)
{
	EhFrame e;
	guint8 cie_insns [8];
	guint8 insns [8];
	int n = 0;

	memset (&e, 0, sizeof (e));
	/* CIE with only a register rule, no def_cfa. */
	cie_insns [0] = (guint8)(DW_CFA_offset | PC_REG);
	cie_insns [1] = 1;
	build_cie (&e, cie_insns, 2);
	insns [n++] = DW_CFA_nop;
	build_fde (&e, 16, insns, n);
	expect_decline ("no-cfa-rule", &e, 16);
}

#ifdef __cplusplus
extern "C"
#endif
int test_llvm_ehframe_main (void);

int
test_llvm_ehframe_main (void)
{
	failures = 0;

	test_restore_to_cie_rule ();
	test_restore_to_no_cie_rule ();
	test_undefined_cancels_cie_rule ();
	test_nested_remember_restore ();
	test_too_many_ops_declines ();
	test_no_cfa_rule_declines ();
	test_malformed_declines ();

	if (failures) {
		printf ("%d failure(s)\n", failures);
		return 1;
	}
	printf ("all ok\n");
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
