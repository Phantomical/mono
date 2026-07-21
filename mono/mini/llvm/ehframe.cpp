/*
 * ehframe.cpp: transcode stock DWARF .eh_frame into mono's unwind ops.
 *
 * The forked LLVM used to emit a mono-format EH frame that decode_llvm_eh_info()
 * consumed. Unmodified LLVM emits a standard DWARF .eh_frame instead, so the
 * per-method unwind description has to be recovered from that.
 *
 * This TRANSCODES rather than byte-copying the CFI program. That is not a
 * stylistic preference - a copier is incorrect here:
 *
 *   - DW_CFA_restore means "revert this register to the rule the CIE
 *     established", so its meaning is only recoverable while the CIE's initial
 *     rules are in hand. A copier that concatenates the CIE and FDE programs -
 *     which is what mono_unwind_decode_fde() does - has already lost them.
 *     mono_unwind_frame() cannot execute the opcode at all: its outer switch on
 *     (*p & 0xc0) handles 0x40/0x80/0x00 and returns FALSE from the default,
 *     and DW_CFA_restore is 0xc0.
 *   - The same goes for DW_CFA_remember_state/restore_state, which mono supports
 *     only to a single level (UnwindState state_stack[1]), and for
 *     DW_CFA_undefined, which mono cannot represent at all.
 *   - Normalizing those away means mono_unwind_frame needs no new opcodes to
 *     consume the result.
 *
 * On x86-64, LLVM 18 happens to unwind epilogues with plain
 * DW_CFA_def_cfa_offset steps rather than DW_CFA_restore, so the simplest
 * shapes would survive a copier. That is not something to rely on: restore,
 * remember_state, restore_state, expression and undefined all occur in real
 * compiler output - they are present in mono's own binary - and any of them
 * reaching mono_unwind_frame is a failure during exception dispatch.
 *
 * Which is the third reason to transcode: anything not representable becomes a
 * FALSE return here, i.e. a compile-time decline that falls the method back to
 * the classic JIT, rather than a wrong or fatal unwind later.
 *
 * Every read below is bounds-checked through Reader. The input comes from our
 * own JIT today, but it is parsed as untrusted: a malformed or truncated section
 * must decline, never walk off the end.
 */

#include "config.h"
#include <glib.h>
#include <string.h>

#include <mono/utils/freebsd-dwarf.h>
#include "mini.h"
#include "mini-unwind.h"
#include "backend.h"

/* Not in mini-unwind.h's DW_EH_PE enum, but legal encodings we may be handed. */
#ifndef DW_EH_PE_udata8
#define DW_EH_PE_udata8 0x04
#endif
/*
 * GCC/LLVM extension recording the argument-stack adjustment across a call.
 * It carries no register or CFA rule, so it is simply skipped.
 */
#ifndef DW_CFA_GNU_args_size
#define DW_CFA_GNU_args_size 0x2e
#endif

/*
 * Storage bound for per-register rules. NUM_DWARF_REGS itself is private to
 * unwind.c, so this is a generous compile-time array bound; the real validity
 * limit is computed at runtime by reg_ok() below.
 */
#define MAX_DWARF_REG 32

/*
 * mono_unwind_ops_encode_full() writes into a fixed guint8 buf[4096] and only
 * checks the bound AFTER the writes, so an over-long op list is a stack smash
 * rather than a diagnostic. The worst case per op is a 5-byte advance_loc plus
 * an opcode and the largest operand encoding it actually uses, i.e. 16 bytes;
 * cap the list well inside 4096 / 16 and decline beyond it.
 */
#define MAX_UNWIND_OPS 128

/* ---------------------------------------------------------------- reading */

/*
 * A bounds-checked cursor. Once ok goes FALSE it stays FALSE and every
 * subsequent read is a no-op returning 0, so callers can decode a whole
 * structure and test ok once at the end rather than after every field.
 */
struct Reader {
	const guint8 *p;
	const guint8 *end;
	gboolean ok;
};

static void
reader_init (Reader *r, const guint8 *start, const guint8 *end)
{
	r->p = start;
	r->end = end;
	r->ok = start <= end;
}

static gboolean
reader_has (Reader *r, gsize n)
{
	if (!r->ok || (gsize)(r->end - r->p) < n) {
		r->ok = FALSE;
		return FALSE;
	}
	return TRUE;
}

static guint8
r_u8 (Reader *r)
{
	if (!reader_has (r, 1))
		return 0;
	return *r->p ++;
}

static guint16
r_u16 (Reader *r)
{
	guint16 v;
	if (!reader_has (r, 2))
		return 0;
	memcpy (&v, r->p, 2);
	r->p += 2;
	return v;
}

static guint32
r_u32 (Reader *r)
{
	guint32 v;
	if (!reader_has (r, 4))
		return 0;
	memcpy (&v, r->p, 4);
	r->p += 4;
	return v;
}

static guint64
r_u64 (Reader *r)
{
	guint64 v;
	if (!reader_has (r, 8))
		return 0;
	memcpy (&v, r->p, 8);
	r->p += 8;
	return v;
}

/*
 * LEB128. Besides bounds, the shift is clamped: an overlong encoding would
 * otherwise reach shift >= 64, where both "res |= x << shift" and the sign
 * extension below are undefined.
 */
static guint64
r_uleb (Reader *r)
{
	guint64 res = 0;
	int shift = 0;

	while (TRUE) {
		guint8 b;

		if (!reader_has (r, 1))
			return 0;
		b = *r->p ++;

		if (shift < 64)
			res |= (guint64)(b & 0x7f) << shift;
		else if (b & 0x7f) {
			/* Significant bits past 64: not representable. */
			r->ok = FALSE;
			return 0;
		}

		if (!(b & 0x80))
			break;
		shift += 7;
		if (shift > 70) {
			r->ok = FALSE;
			return 0;
		}
	}
	return res;
}

static gint64
r_sleb (Reader *r)
{
	gint64 res = 0;
	int shift = 0;
	guint8 b = 0;

	while (TRUE) {
		if (!reader_has (r, 1))
			return 0;
		b = *r->p ++;

		if (shift < 64)
			res |= (gint64)(b & 0x7f) << shift;
		else if ((b & 0x7f) != 0 && (b & 0x7f) != 0x7f) {
			r->ok = FALSE;
			return 0;
		}

		shift += 7;
		if (!(b & 0x80))
			break;
		if (shift > 70) {
			r->ok = FALSE;
			return 0;
		}
	}

	if (shift < 64 && (b & 0x40))
		res |= -((gint64)1 << shift);

	return res;
}

/* --------------------------------------------------------------- rules */

/*
 * Only dwarf regs mono knows about survive its unwinder: mono_unwind_frame()
 * silently DISCARDS a DW_CFA_offset for anything at or above NUM_DWARF_REGS.
 * Silently dropping a saved register is precisely the sort of thing that turns
 * into an unexplainable crash later, so a rule for such a register makes us
 * decline the method instead.
 *
 * The highest register mono maps is the return-address column (rip = 16 on
 * amd64), which is what mono_unwind_get_dwarf_pc_reg() reports, so that + 1 is
 * the count. Anything above it - notably the XMM registers - is declined. On
 * SysV amd64 there are no callee-saved XMMs, so this is a guard rather than a
 * workaround.
 */
static gboolean
reg_ok (guint64 reg)
{
	return reg <= (guint64) mono_unwind_get_dwarf_pc_reg () && reg < MAX_DWARF_REG;
}

enum RuleKind {
	/* No rule: the register is not recoverable from this frame. */
	RULE_UNDEFINED = 0,
	/* Register still holds its caller value. */
	RULE_SAME,
	/* Register was spilled to cfa + offset. */
	RULE_OFFSET
};

struct RegRule {
	RuleKind kind;
	int offset; /* byte offset from the CFA, already scaled by data_align */
};

struct CfiState {
	int cfa_reg;    /* dwarf register number, -1 if unset */
	int cfa_offset;
	RegRule regs [MAX_DWARF_REG];
};

/* Parsed CIE fields we care about. */
struct CieInfo {
	int code_align;
	int data_align;
	int return_reg;
	guint8 fde_ptr_encoding;       /* from the 'R' augmentation */
	gboolean has_augmentation_len; /* 'z' */
	const guint8 *cfi;             /* start of the initial instructions */
	const guint8 *cfi_end;
};

/*
 * Convert a factored DWARF offset into the byte offset mono stores, rejecting
 * anything that will not survive mono's encoding.
 *
 * mono_unwind_ops_encode_full() re-factors with TRUNCATING integer division
 * (op->val / DWARF_DATA_ALIGN), so the round trip only cancels for exact
 * multiples of mono's own alignment - which is why the caller requires the CIE's
 * data_align to equal it. A positive byte offset is rejected outright: mono
 * encodes the factored value with encode_uleb128(), which takes a guint32, so a
 * register saved above the CFA would encode as a huge unsigned value that
 * mono_unwind_frame() multiplies back out and dereferences - a wild read in
 * async-signal context during GC stack scanning.
 */
static gboolean
factored_to_byte_offset (gint64 factored, int data_align, int *out)
{
	gint64 bytes;

	if (data_align == 0)
		return FALSE;

	bytes = factored * (gint64) data_align;

	if (bytes > 0 || bytes < G_MININT32)
		return FALSE;
	if ((bytes % data_align) != 0)
		return FALSE;

	*out = (int) bytes;
	return TRUE;
}

/*
 * Decode an FDE initial_location. LLVM emits DW_EH_PE_pcrel|DW_EH_PE_sdata4
 * (0x1b); absolute and pcrel/sdata8 are accepted too. Anything else declines -
 * including DW_EH_PE_indirect (bit 7), which would otherwise yield the address
 * OF a pointer rather than the pointer itself.
 */
static gboolean
decode_fde_pointer (guint8 encoding, Reader *r, const guint8 **out_addr)
{
	const guint8 *base = r->p;
	gint64 val;

	if (encoding & DW_EH_PE_indirect)
		return FALSE;

	switch (encoding & 0x0f) {
	case DW_EH_PE_absptr:
		if (sizeof (gpointer) == 8)
			val = (gint64) r_u64 (r);
		else
			val = (gint32) r_u32 (r);
		break;
	case DW_EH_PE_sdata4:
		val = (gint32) r_u32 (r);
		break;
	case DW_EH_PE_udata4:
		val = (gint64) r_u32 (r);
		break;
	case DW_EH_PE_sdata8:
	case DW_EH_PE_udata8:
		val = (gint64) r_u64 (r);
		break;
	default:
		return FALSE;
	}

	if (!r->ok)
		return FALSE;

	switch (encoding & 0x70) {
	case DW_EH_PE_absptr:
		*out_addr = (const guint8*)(gsize) val;
		break;
	case DW_EH_PE_pcrel:
		*out_addr = base + val;
		break;
	default:
		return FALSE;
	}

	return TRUE;
}

/* Skip the value an encoding describes, without interpreting it. */
static gboolean
skip_encoded (guint8 encoding, Reader *r)
{
	gsize n;

	switch (encoding & 0x0f) {
	case DW_EH_PE_absptr:
		n = sizeof (gpointer);
		break;
	case DW_EH_PE_sdata4:
	case DW_EH_PE_udata4:
		n = 4;
		break;
	case DW_EH_PE_sdata8:
	case DW_EH_PE_udata8:
		n = 8;
		break;
	default:
		return FALSE;
	}

	if (!reader_has (r, n))
		return FALSE;
	r->p += n;
	return TRUE;
}

static void
emit_op (GSList **ops, int *count, guint32 when, int op, int dwarf_reg, int val)
{
	int hwreg = dwarf_reg >= 0 ? mono_dwarf_reg_to_hw_reg (dwarf_reg) : 0;

	*ops = g_slist_append (*ops, mono_create_unwind_op (when, op, hwreg, val));
	(*count) ++;
}

/* Emit whatever concretely represents RULE for REG at WHEN. */
static void
emit_rule (GSList **ops, int *count, guint32 when, int reg, const RegRule *rule)
{
	if (rule->kind == RULE_OFFSET)
		emit_op (ops, count, when, DW_CFA_offset, reg, rule->offset);
	else
		/*
		 * Not saved by this frame. same_value leaves mono's locations[] as
		 * LOC_SAME, so the register is not restored - which is what both
		 * "reverted an epilogue's spill" and "explicitly undefined" mean as far
		 * as mono is able to express them.
		 */
		emit_op (ops, count, when, DW_CFA_same_value, reg, 0);
}

/*
 * Execute one CFI program. If OPS is NULL the run only updates STATE (used for
 * the CIE's initial instructions); otherwise every state change is also appended
 * to OPS as a MonoUnwindOp.
 *
 * INITIAL supplies the CIE rules that DW_CFA_restore reverts to; it is NULL
 * while the CIE itself is being interpreted.
 */
static gboolean
run_cfi (const guint8 *start, const guint8 *end, CieInfo *cie, CfiState *state,
         const CfiState *initial, GSList **ops, int *count, guint32 code_len)
{
	Reader r;
	guint32 loc = 0;
	/*
	 * DW_CFA_remember_state/restore_state are simulated here and never emitted:
	 * mono_unwind_frame()'s state stack is literally UnwindState state_stack[1]
	 * and fails outright at depth >= 2. Materializing the restored rules instead
	 * removes that limit.
	 */
	GQueue *stack = g_queue_new ();
	gboolean ok = TRUE;

	reader_init (&r, start, end);

	while (ok && r.ok && r.p < r.end) {
		guint8 b = r_u8 (&r);
		int primary = b & 0xc0;
		int operand = b & 0x3f;

		if (!r.ok)
			break;
		if (ops && *count > MAX_UNWIND_OPS) {
			ok = FALSE;
			break;
		}

		if (primary == DW_CFA_advance_loc) {
			loc += operand * cie->code_align;
		} else if (primary == DW_CFA_offset) {
			gint64 factored = (gint64) r_uleb (&r);
			int bytes;

			if (!r.ok) break;
			if (!reg_ok (operand)) { ok = FALSE; break; }
			if (!factored_to_byte_offset (factored, cie->data_align, &bytes)) { ok = FALSE; break; }
			state->regs [operand].kind = RULE_OFFSET;
			state->regs [operand].offset = bytes;
			if (ops)
				emit_op (ops, count, loc, DW_CFA_offset, operand, bytes);
		} else if (primary == DW_CFA_restore) {
			/*
			 * "Revert to the CIE rule." This is the opcode a byte-copier gets
			 * wrong, and that mono_unwind_frame() cannot execute, so resolve it
			 * to the concrete rule here.
			 */
			if (!reg_ok (operand) || !initial) { ok = FALSE; break; }
			state->regs [operand] = initial->regs [operand];
			if (ops)
				emit_rule (ops, count, loc, operand, &state->regs [operand]);
		} else {
			/* primary == 0: extended opcode */
			switch (b) {
			case DW_CFA_nop:
				break;
			case DW_CFA_set_loc:
				/* Absolute location; we only track offsets, so decline. */
				ok = FALSE;
				break;
			case DW_CFA_advance_loc1:
				loc += r_u8 (&r) * cie->code_align;
				break;
			case DW_CFA_advance_loc2:
				loc += r_u16 (&r) * cie->code_align;
				break;
			case DW_CFA_advance_loc4:
				loc += r_u32 (&r) * cie->code_align;
				break;
			case DW_CFA_def_cfa: {
				guint64 reg = r_uleb (&r);
				guint64 off = r_uleb (&r);
				if (!r.ok) break;
				if (!reg_ok (reg) || off > G_MAXINT32) { ok = FALSE; break; }
				state->cfa_reg = (int) reg;
				state->cfa_offset = (int) off;
				if (ops)
					emit_op (ops, count, loc, DW_CFA_def_cfa, (int) reg, (int) off);
				break;
			}
			case DW_CFA_def_cfa_register: {
				guint64 reg = r_uleb (&r);
				if (!r.ok) break;
				if (!reg_ok (reg)) { ok = FALSE; break; }
				state->cfa_reg = (int) reg;
				if (ops)
					emit_op (ops, count, loc, DW_CFA_def_cfa_register, (int) reg, 0);
				break;
			}
			case DW_CFA_def_cfa_offset: {
				guint64 off = r_uleb (&r);
				if (!r.ok) break;
				if (off > G_MAXINT32) { ok = FALSE; break; }
				state->cfa_offset = (int) off;
				if (ops)
					emit_op (ops, count, loc, DW_CFA_def_cfa_offset, -1, (int) off);
				break;
			}
			case DW_CFA_def_cfa_sf: {
				guint64 reg = r_uleb (&r);
				gint64 off = r_sleb (&r);
				gint64 bytes = off * (gint64) cie->data_align;
				if (!r.ok) break;
				/* mono encodes the CFA offset unsigned, so it must be >= 0. */
				if (!reg_ok (reg) || bytes < 0 || bytes > G_MAXINT32) { ok = FALSE; break; }
				state->cfa_reg = (int) reg;
				state->cfa_offset = (int) bytes;
				if (ops)
					emit_op (ops, count, loc, DW_CFA_def_cfa, (int) reg, (int) bytes);
				break;
			}
			case DW_CFA_def_cfa_offset_sf: {
				gint64 off = r_sleb (&r);
				gint64 bytes = off * (gint64) cie->data_align;
				if (!r.ok) break;
				if (bytes < 0 || bytes > G_MAXINT32) { ok = FALSE; break; }
				state->cfa_offset = (int) bytes;
				if (ops)
					emit_op (ops, count, loc, DW_CFA_def_cfa_offset, -1, (int) bytes);
				break;
			}
			case DW_CFA_offset_extended:
			case DW_CFA_offset_extended_sf: {
				guint64 reg = r_uleb (&r);
				gint64 factored = (b == DW_CFA_offset_extended)
					? (gint64) r_uleb (&r) : r_sleb (&r);
				int bytes;
				if (!r.ok) break;
				if (!reg_ok (reg)) { ok = FALSE; break; }
				if (!factored_to_byte_offset (factored, cie->data_align, &bytes)) { ok = FALSE; break; }
				state->regs [reg].kind = RULE_OFFSET;
				state->regs [reg].offset = bytes;
				if (ops)
					emit_op (ops, count, loc, DW_CFA_offset, (int) reg, bytes);
				break;
			}
			case DW_CFA_restore_extended: {
				guint64 reg = r_uleb (&r);
				if (!r.ok) break;
				if (!reg_ok (reg) || !initial) { ok = FALSE; break; }
				state->regs [reg] = initial->regs [reg];
				if (ops)
					emit_rule (ops, count, loc, (int) reg, &state->regs [reg]);
				break;
			}
			case DW_CFA_same_value: {
				guint64 reg = r_uleb (&r);
				if (!r.ok) break;
				if (!reg_ok (reg)) { ok = FALSE; break; }
				state->regs [reg].kind = RULE_SAME;
				if (ops)
					emit_op (ops, count, loc, DW_CFA_same_value, (int) reg, 0);
				break;
			}
			case DW_CFA_undefined: {
				guint64 reg = r_uleb (&r);
				if (!r.ok) break;
				if (!reg_ok (reg)) { ok = FALSE; break; }
				/*
				 * "This register is not recoverable from here." mono has no such
				 * rule, but it MUST still be emitted rather than merely recorded:
				 * the CIE's rules are restated at offset 0, and on x86-64 those
				 * include "pc at cfa-8". An FDE declaring the return column
				 * undefined - which is how a frame says "stop unwinding here" -
				 * would otherwise leave that stale, and mono_unwind_frame would
				 * load a return address from a slot that no longer holds one.
				 * same_value is the closest mono can express: not restored.
				 */
				state->regs [reg].kind = RULE_UNDEFINED;
				if (ops)
					emit_op (ops, count, loc, DW_CFA_same_value, (int) reg, 0);
				break;
			}
			case DW_CFA_remember_state: {
				CfiState *saved = g_new (CfiState, 1);
				*saved = *state;
				g_queue_push_head (stack, saved);
				break;
			}
			case DW_CFA_restore_state: {
				CfiState *saved = (CfiState *) g_queue_pop_head (stack);
				if (!saved) { ok = FALSE; break; }
				/*
				 * A restored state with no CFA rule cannot be expressed, and
				 * emitting def_cfa with reg -1 would fabricate a rule on r0.
				 */
				if (saved->cfa_reg == -1) { g_free (saved); ok = FALSE; break; }
				if (ops) {
					/* Materialize the differences; emit neither opcode. */
					if (saved->cfa_reg != state->cfa_reg || saved->cfa_offset != state->cfa_offset)
						emit_op (ops, count, loc, DW_CFA_def_cfa, saved->cfa_reg, saved->cfa_offset);
					for (int i = 0; i < MAX_DWARF_REG; ++i) {
						if (saved->regs [i].kind == state->regs [i].kind &&
						    saved->regs [i].offset == state->regs [i].offset)
							continue;
						emit_rule (ops, count, loc, i, &saved->regs [i]);
					}
				}
				*state = *saved;
				g_free (saved);
				break;
			}
			case DW_CFA_GNU_args_size:
				/* Stack adjustment for calls; irrelevant to mono's unwinder. */
				r_uleb (&r);
				break;
			/*
			 * Expressions would need a DWARF expression evaluator in async-signal
			 * context. Decline instead - the method falls back to the classic JIT.
			 */
			case DW_CFA_def_cfa_expression:
			case DW_CFA_expression:
			case DW_CFA_val_expression:
			default:
				ok = FALSE;
				break;
			}
		}

		/*
		 * Checked after every opcode, not only the extended ones: advance_loc is
		 * the common case and is exactly what can run past the function.
		 */
		if (loc > code_len)
			ok = FALSE;
	}

	if (!r.ok)
		ok = FALSE;

	while (!g_queue_is_empty (stack))
		g_free (g_queue_pop_head (stack));
	g_queue_free (stack);

	return ok;
}

/* Parse a CIE whose contents span [start, end). Returns FALSE if unsupported. */
static gboolean
parse_cie (const guint8 *start, const guint8 *end, CieInfo *out)
{
	Reader r;
	guint8 version;
	const guint8 *aug_str;
	gsize aug_str_len = 0;

	reader_init (&r, start, end);

	version = r_u8 (&r);
	if (!r.ok || (version != 1 && version != 3))
		return FALSE;

	/*
	 * Bounded scan for the augmentation string's terminator. A plain strlen()
	 * here would run off a CIE truncated after the version byte.
	 */
	aug_str = r.p;
	while (TRUE) {
		guint8 c = r_u8 (&r);
		if (!r.ok)
			return FALSE;
		if (c == '\0')
			break;
		aug_str_len ++;
		if (aug_str_len > 16) /* no legitimate augmentation is this long */
			return FALSE;
	}

	out->code_align = (int) r_uleb (&r);
	out->data_align = (int) r_sleb (&r);
	out->return_reg = (version == 1) ? r_u8 (&r) : (int) r_uleb (&r);
	if (!r.ok)
		return FALSE;

	out->fde_ptr_encoding = DW_EH_PE_absptr;
	out->has_augmentation_len = FALSE;

	if (aug_str_len > 0 && aug_str [0] == 'z') {
		guint64 aug_len = r_uleb (&r);
		const guint8 *aug_end;

		if (!r.ok || aug_len > (guint64)(r.end - r.p))
			return FALSE;
		out->has_augmentation_len = TRUE;
		aug_end = r.p + aug_len;

		for (gsize i = 1; i < aug_str_len; ++i) {
			switch (aug_str [i]) {
			case 'R':
				out->fde_ptr_encoding = r_u8 (&r);
				break;
			case 'P': {
				guint8 enc = r_u8 (&r);
				if (!r.ok || !skip_encoded (enc, &r))
					return FALSE;
				break;
			}
			case 'L':
				r_u8 (&r); /* LSDA encoding; the LSDA itself is not needed here */
				break;
			case 'S':
				/* Signal frame; no extra data. */
				break;
			default:
				return FALSE;
			}
			if (!r.ok)
				return FALSE;
		}
		r.p = aug_end;
	} else if (aug_str_len > 0) {
		/* Augmentation without a length field: cannot skip it safely. */
		return FALSE;
	}

	if (!r.ok)
		return FALSE;

	out->cfi = r.p;
	out->cfi_end = end;
	return TRUE;
}

gboolean
mono_llvm_eh_frame_to_unwind_ops (guint8 *eh_frame, guint32 eh_frame_size,
                                  gpointer code_start, guint32 code_len,
                                  GSList **out_ops)
{
	const guint8 *section_end;
	Reader sec;

	if (!out_ops)
		return FALSE;
	*out_ops = NULL;

	if (!eh_frame || eh_frame_size < 4)
		return FALSE;

	section_end = eh_frame + eh_frame_size;
	reader_init (&sec, eh_frame, section_end);

	while (sec.ok && sec.p + 4 <= section_end) {
		guint32 length;
		const guint8 *entry_end;
		const guint8 *id_pos;
		guint32 id;

		length = r_u32 (&sec);
		if (!sec.ok)
			return FALSE;

		/* A zero-length entry terminates the section. */
		if (length == 0)
			break;
		/* 64-bit DWARF is not produced by LLVM here; decline rather than guess. */
		if (length == 0xffffffff)
			return FALSE;
		if (length > (guint32)(section_end - sec.p))
			return FALSE;

		entry_end = sec.p + length;
		id_pos = sec.p;
		/*
		 * The id must be read through the bounds-checked reader: an entry
		 * claiming length 1 satisfies the check above but has only one byte.
		 */
		id = r_u32 (&sec);
		if (!sec.ok)
			return FALSE;

		if (id == 0) {
			/* CIE: skip; FDEs locate their own CIE below. */
			sec.p = entry_end;
			continue;
		}

		/*
		 * FDE. In .eh_frame the second word is the distance BACK from its own
		 * position to the CIE (unlike .debug_frame, where it is a section
		 * offset).
		 */
		{
			CieInfo cie;
			const guint8 *cie_start;
			const guint8 *cie_contents_end;
			const guint8 *pc_begin = NULL;
			Reader fde;
			CfiState initial, state;
			GSList *ops = NULL;
			int count = 0;

			if ((gsize) id > (gsize)(id_pos - eh_frame))
				return FALSE;
			cie_start = id_pos - id;

			{
				Reader cr;
				guint32 cie_len;

				reader_init (&cr, cie_start, section_end);
				cie_len = r_u32 (&cr);
				if (!cr.ok || cie_len == 0 || cie_len == 0xffffffff)
					return FALSE;
				/*
				 * The FDE's own extent was bounded above; the CIE is reached by a
				 * backward offset and needs the same check, or a bogus pointer
				 * lets parse_cie succeed on unrelated memory and run_cfi
				 * interpret megabytes of it as a CFI program.
				 */
				if (cie_len > (guint32)(section_end - cr.p))
					return FALSE;
				cie_contents_end = cr.p + cie_len;
				/* Skip the CIE id field; parse_cie starts at the version byte. */
				if (!reader_has (&cr, 4))
					return FALSE;
				cr.p += 4;
				if (!parse_cie (cr.p, cie_contents_end, &cie))
					return FALSE;
			}

			reader_init (&fde, sec.p, entry_end);
			if (!decode_fde_pointer (cie.fde_ptr_encoding, &fde, &pc_begin))
				return FALSE;
			/* pc_range, same size class as the pointer, but never relocated. */
			if (!skip_encoded (cie.fde_ptr_encoding, &fde))
				return FALSE;

			if (cie.has_augmentation_len) {
				guint64 aug_len = r_uleb (&fde);
				if (!fde.ok || aug_len > (guint64)(fde.end - fde.p))
					return FALSE;
				fde.p += aug_len;
			}
			if (!fde.ok)
				return FALSE;

			if (pc_begin != (const guint8*) code_start) {
				/*
				 * A different function in the same module - the GC safepoint
				 * poll rides along with the method, so this is normal.
				 */
				sec.p = entry_end;
				continue;
			}

			/*
			 * mono re-factors register offsets with TRUNCATING division by its
			 * own DWARF_DATA_ALIGN, so that must be the alignment they were
			 * produced with or slots silently shift (a -4 CIE would turn cfa-12
			 * into cfa-8). code_align is applied to every advance_loc, so any
			 * positive value works; LLVM emits 1 on x86-64.
			 */
			if (cie.code_align <= 0 || cie.data_align >= 0)
				return FALSE;
			if (cie.data_align != mono_unwind_get_dwarf_data_align ())
				return FALSE;
			/*
			 * The return-address column has to be the one mono restores as the
			 * caller's pc, or the unwound frame would resume at the wrong place.
			 */
			if (cie.return_reg != mono_unwind_get_dwarf_pc_reg ())
				return FALSE;

			memset (&initial, 0, sizeof (initial));
			initial.cfa_reg = -1;
			if (!run_cfi (cie.cfi, cie.cfi_end, &cie, &initial, NULL, NULL, NULL, code_len))
				return FALSE;

			/*
			 * Without a CFA rule there is nothing to unwind with. Returning a
			 * well-formed-looking op list here would defer the failure to
			 * exception dispatch; decline at compile time instead.
			 */
			if (initial.cfa_reg == -1)
				return FALSE;

			/*
			 * Restate the CIE rules at offset 0 so the descriptor stands alone:
			 * mono's unwinder is handed only this op list, never the CIE.
			 */
			state = initial;
			emit_op (&ops, &count, 0, DW_CFA_def_cfa, initial.cfa_reg, initial.cfa_offset);
			for (int i = 0; i < MAX_DWARF_REG; ++i) {
				if (initial.regs [i].kind == RULE_OFFSET)
					emit_op (&ops, &count, 0, DW_CFA_offset, i, initial.regs [i].offset);
			}

			if (!run_cfi (fde.p, entry_end, &cie, &state, &initial, &ops, &count, code_len) ||
			    count > MAX_UNWIND_OPS) {
				mono_free_unwind_info (ops);
				return FALSE;
			}

			*out_ops = ops;
			return TRUE;
		}
	}

	/*
	 * No FDE for this function. LLVM omits one for a nounwind leaf. We cannot
	 * tell that apart from a lookup failure here, and publishing a frame with no
	 * unwind information is exactly the unsafe outcome this exists to prevent,
	 * so decline and let the classic JIT take the method.
	 */
	return FALSE;
}
