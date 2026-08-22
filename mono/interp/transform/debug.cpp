/**
 * \file
 * \brief What the transform prints, and what it hands the debugger.
 *
 * The instruction dumps MONO_VERBOSE_METHOD asks for, the bytecode-offset to
 * IL-offset map a stack trace reads, and the sequence points the debugger
 * agent steps over.
 */

#include "config.h"

#include <mono/metadata/class-internals.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/profiler-private.h>
#include <mono/metadata/seq-points-data.h>
#include <mono/utils/unlocked.h>

#include <mono/mini/mini-runtime.h>

#include "mintops.hpp"
#include "runtime/internals.hpp"
#include "transform.hpp"
#include "internal.hpp"

namespace mono::interp {

/// Formats a decoded instruction's operand bytes for printing.
///
/// \param ins        the instruction being dumped, or null when \p data
///                    points into the final compacted code instead. A branch
///                    target prints as a block number in the first case and
///                    as a raw offset in the second.
/// \param ins_offset  this instruction's own offset, added to a branch's
///                    operand to print its target when \p ins is null.
/// \param data        the address holding the instruction's operand bytes.
/// \param opcode      the opcode that decides how \p data is read.
///
/// Returns a newly allocated string the caller must free.
char *
dump_interp_ins_data (InterpInst *ins, gint32 ins_offset, const guint16 *data, guint16 opcode)
{
	GString *str = g_string_new ("");
	guint32 token;
	int target;

	switch (opargtype (opcode)) {
	case MintOpNoArgs:
		break;
	case MintOpUShortInt:
		g_string_append_printf (str, " %u", *(guint16 *) data);
		break;
	case MintOpTwoShorts:
		g_string_append_printf (str, " %u,%u", *(guint16 *) data, *(guint16 *) (data + 1));
		break;
	case MintOpShortAndInt:
		g_string_append_printf (str, " %u,%u", *(guint16 *) data, (guint32) READ32 (data + 1));
		break;
	case MintOpShortInt:
		g_string_append_printf (str, " %d", *(gint16 *) data);
		break;
	case MintOpClassToken:
	case MintOpMethodToken:
	case MintOpFieldToken:
		token = *(guint16 *) data;
		g_string_append_printf (str, " %u", token);
		break;
	case MintOpInt:
		g_string_append_printf (str, " %d", (gint32) READ32 (data));
		break;
	case MintOpLongInt:
		g_string_append_printf (str, " %" PRId64, (gint64) READ64 (data));
		break;
	case MintOpFloat: {
		gint32 tmp = READ32 (data);
		g_string_append_printf (str, " %g", *(float *) &tmp);
		break;
	}
	case MintOpDouble: {
		gint64 tmp = READ64 (data);
		g_string_append_printf (str, " %g", *(double *) &tmp);
		break;
	}
	case MintOpShortBranch:
		if (ins) {
			/* the target block is already resolved */
			g_string_append_printf (str, " BB%d", ins->info.target_bb->index);
		} else {
			target = ins_offset + *(gint16 *) data;
			g_string_append_printf (str, " IR_%04x", target);
		}
		break;
	case MintOpBranch:
		if (ins) {
			g_string_append_printf (str, " BB%d", ins->info.target_bb->index);
		} else {
			target = ins_offset + (gint32) READ32 (data);
			g_string_append_printf (str, " IR_%04x", target);
		}
		break;
	case MintOpSwitch: {
		int sval = (gint32) READ32 (data);
		int i;
		g_string_append_printf (str, "(");
		gint32 p = 2;
		for (i = 0; i < sval; ++i) {
			if (i > 0)
				g_string_append_printf (str, ", ");
			if (ins) {
				g_string_append_printf (str, "BB%d", ins->info.target_bb_table[i]->index);
			} else {
				g_string_append_printf (str, "IR_%04x", (gint32) READ32 (data + p));
			}
			p += 2;
		}
		g_string_append_printf (str, ")");
		break;
	}
	default:
		g_string_append_printf (str, "unknown arg type\n");
	}

	return g_string_free (str, FALSE);
}

void
dump_interp_code (FILE *out, const guint16 *start, const guint16 *end)
{
	const guint16 *p = start;
	while (p < end) {
		mono_interp_dis_mintop (out, p, start);
		p = dis_mintop_len (p);
	}
}

static void
dump_interp_inst_no_newline (InterpInst *ins)
{
	int opcode = ins->opcode;
	g_print ("IL_%04x: %-14s", ins->il_offset, opname (opcode));

	if (num_dregs (opcode) == MINT_CALL_ARGS)
		g_print (" [call_args %d <-", ins->dreg);
	else if (num_dregs (opcode) > 0)
		g_print (" [%d <-", ins->dreg);
	else
		g_print (" [nil <-");

	if (num_sregs (opcode) > 0) {
		for (int i = 0; i < num_sregs (opcode); i++)
			g_print (" %d", ins->sregs[i]);
		g_print ("],");
	} else {
		g_print (" nil],");
	}

	if (opcode == MINT_LDLOCA_S) {
		// MINT_LDLOCA_S is special: it has data in sregs [0] but reports no sregs.
		g_print (" %d", ins->sregs[0]);
	} else {
		char *descr = dump_interp_ins_data (ins, ins->il_offset, &ins->data[0], ins->opcode);
		g_print ("%s", descr);
		g_free (descr);
	}
}

void
dump_interp_inst (InterpInst *ins)
{
	dump_interp_inst_no_newline (ins);
	g_print ("\n");
}

static G_GNUC_UNUSED void
dump_interp_bb (InterpBasicBlock *bb)
{
	g_print ("BB%d:\n", bb->index);
	for (InterpInst *ins : *bb)
		dump_interp_inst (ins);
}

static guint8 *
encode_uleb128 (guint32 value, guint8 *p)
{
	do {
		guint8 b = value & 0x7f;

		value >>= 7;
		if (value)
			b |= 0x80;
		*p++ = b;
	} while (value);

	return p;
}

/// Keeps the bytecode-offset to IL-offset map the transform built, so a stack
/// trace can report an IL offset for a frame of this method.
void
TransformData::interp_save_line_numbers (InterpMethod *rtm,
                                         const std::vector<MonoDebugLineNumberEntry> &line_numbers)
{
	if (line_numbers.empty ())
		return;

	/* Two varints an entry, each at most five bytes. */
	guint8 *buf = g_new (guint8, line_numbers.size () * 10);
	guint8 *p = buf;
	guint32 prev_native = 0;
	gint32 prev_il = 0;

	for (const MonoDebugLineNumberEntry &lne : line_numbers) {
		gint32 il_delta = (gint32) lne.il_offset - prev_il;

		p = encode_uleb128 (lne.native_offset - prev_native, p);
		/* Zigzag: emission order is the bytecode's, so the IL offset can step back. */
		p = encode_uleb128 ((guint32) ((il_delta << 1) ^ (il_delta >> 31)), p);

		prev_native = lne.native_offset;
		prev_il = (gint32) lne.il_offset;
	}

	rtm->line_numbers_size = (guint32) (p - buf);
	rtm->line_numbers = (guint8 *) mono_mem_manager_alloc0 (mem_manager, rtm->line_numbers_size);
	memcpy (rtm->line_numbers, buf, rtm->line_numbers_size);
	g_free (buf);

	mono_atomic_fetch_add_i32 (&mono_interp_stats.line_numbers_size,
	                           (gint32) rtm->line_numbers_size);
}

void
TransformData::interp_save_debug_info (InterpMethod *rtm, MonoMethodHeader *header,
                                       const std::vector<MonoDebugLineNumberEntry> &line_numbers)
{
	MonoDebugMethodJitInfo *dinfo;
	int i;

	if (!mono_debug_enabled ())
		return;

	/*
	 * We save the debug info in the same way the JIT does it, treating the interpreter IR as the native code.
	 */

	dinfo = g_new0 (MonoDebugMethodJitInfo, 1);
	dinfo->num_params = rtm->param_count;
	dinfo->params = g_new0 (MonoDebugVarInfo, dinfo->num_params);
	dinfo->num_locals = header->num_locals;
	dinfo->locals = g_new0 (MonoDebugVarInfo, header->num_locals);
	dinfo->code_start = (guint8 *) rtm->code;
	dinfo->code_size = new_code_end - new_code;
	dinfo->epilogue_begin = 0;
	dinfo->has_var_info = TRUE;
	dinfo->num_line_numbers = (int) line_numbers.size ();
	dinfo->line_numbers = g_new0 (MonoDebugLineNumberEntry, dinfo->num_line_numbers);

	for (i = 0; i < dinfo->num_params; i++) {
		MonoDebugVarInfo *var = &dinfo->params[i];
		var->type = rtm->param_types[i];
	}
	for (i = 0; i < dinfo->num_locals; i++) {
		MonoDebugVarInfo *var = &dinfo->locals[i];
		var->type = mono_metadata_type_dup (NULL, header->locals[i]);
	}

	std::copy (line_numbers.begin (), line_numbers.end (), dinfo->line_numbers);
	mono_debug_add_method (rtm->method, dinfo, rtm->domain);

	mono_debug_free_method_jit_info (dinfo);
}

static void
insert_pred_seq_point (SeqPoint *last_sp, SeqPoint *sp, GSList **next)
{
	GSList *l;
	int src_index = last_sp->next_offset;
	int dst_index = sp->next_offset;

	/* bb->in_bb can contain duplicates */
	for (l = next[src_index]; l; l = l->next)
		if (GPOINTER_TO_UINT (l->data) == dst_index)
			break;
	if (!l)
		next[src_index] = g_slist_append (next[src_index], GUINT_TO_POINTER (dst_index));
}

void
TransformData::recursively_make_pred_seq_points (InterpBasicBlock *bb)
{
	SeqPoint **const MONO_SEQ_SEEN_LOOP = (SeqPoint **) GINT_TO_POINTER (-1);

	GArray *predecessors = g_array_new (FALSE, TRUE, sizeof (gpointer));
	GHashTable *seen = g_hash_table_new_full (g_direct_hash, NULL, NULL, NULL);

	// Insert/remove sentinel into the memoize table to detect loops containing bb
	bb->pred_seq_points = MONO_SEQ_SEEN_LOOP;

	for (int i = 0; i < bb->in_count; ++i) {
		InterpBasicBlock *in_bb = bb->in_bb[i];

		// This bb has the last seq point, append it and continue
		if (in_bb->last_seq_point != NULL) {
			predecessors = g_array_append_val (predecessors, in_bb->last_seq_point);
			continue;
		}

		// We've looped or handled this before, exit early.
		// No last sequence points to find.
		if (in_bb->pred_seq_points == MONO_SEQ_SEEN_LOOP)
			continue;

		if (in_bb == entry_bb)
			continue;

		// Take sequence points from incoming basic blocks
		if (in_bb->pred_seq_points == NULL)
			recursively_make_pred_seq_points (in_bb);

		// Union sequence points with incoming bb's
		for (int i = 0; i < in_bb->num_pred_seq_points; i++) {
			if (!g_hash_table_lookup (seen, in_bb->pred_seq_points[i])) {
				g_array_append_val (predecessors, in_bb->pred_seq_points[i]);
				g_hash_table_insert (seen, in_bb->pred_seq_points[i],
				                     (gpointer) &MONO_SEQ_SEEN_LOOP);
			}
		}
	}

	g_hash_table_destroy (seen);

	if (predecessors->len != 0) {
		bb->pred_seq_points = arena.create_array<SeqPoint *> (predecessors->len);
		bb->num_pred_seq_points = predecessors->len;

		for (int newer = 0; newer < bb->num_pred_seq_points; newer++) {
			bb->pred_seq_points[newer] = (SeqPoint *) g_array_index (predecessors, gpointer, newer);
		}
	}

	g_array_free (predecessors, TRUE);
}

void
TransformData::collect_pred_seq_points (InterpBasicBlock *bb, SeqPoint *seqp, GSList **next)
{
	// If bb has no memoized predecessor set, compute one before linking seqp.
	if (bb->pred_seq_points == NULL && bb != entry_bb)
		recursively_make_pred_seq_points (bb);

	for (int i = 0; i < bb->num_pred_seq_points; i++)
		insert_pred_seq_point (bb->pred_seq_points[i], seqp, next);

	return;
}

void
TransformData::save_seq_points (MonoJitInfo *jinfo)
{
	GByteArray *array;
	int seq_info_size;
	MonoSeqPointInfo *info;
	GSList **next = NULL;

	if (!gen_sdb_seq_points)
		return;

	/*
	 * For each sequence point, compute the list of sequence points immediately
	 * following it. The debugger agent needs this list to implement 'step over'.
	 */
	for (size_t i = 0; i < seq_points.size (); ++i)
		/* Store the seq point index here temporarily */
		seq_points[i]->next_offset = (int) i;

	next = arena.create_array<GSList *> (seq_points.size ());
	for (InterpBasicBlock *bb : basic_blocks) {
		GSList *bb_seq_points = g_slist_reverse (bb->seq_points);
		SeqPoint *last = NULL;
		for (GSList *l = bb_seq_points; l; l = l->next) {
			SeqPoint *sp = (SeqPoint *) l->data;

			if (sp->il_offset == METHOD_ENTRY_IL_OFFSET || sp->il_offset == METHOD_EXIT_IL_OFFSET)
				/* Used to implement method entry/exit events */
				continue;

			if (last != NULL) {
				/* Link with the previous seq point in the same bb */
				next[last->next_offset] = g_slist_append_mempool (
					arena.pool (), next[last->next_offset], GINT_TO_POINTER (sp->next_offset));
			} else {
				/* Link with the predecessor blocks' seq points */
				collect_pred_seq_points (bb, sp, next);
			}
			last = sp;
		}
	}

	/* Serialize the seq points into a byte array */
	array = g_byte_array_new ();
	SeqPoint zero_seq_point = {0};
	SeqPoint *last_seq_point = &zero_seq_point;
	for (size_t i = 0; i < seq_points.size (); ++i) {
		SeqPoint *sp = seq_points[i];

		sp->next_offset = 0;
		if (mono_seq_point_info_add_seq_point (array, sp, last_seq_point, next[i], TRUE))
			last_seq_point = sp;
	}

	if (verbose_level) {
		g_print ("\nSEQ POINT MAP FOR %s: \n", method->name);

		for (size_t i = 0; i < seq_points.size (); ++i) {
			SeqPoint *sp = seq_points[i];

			if (!next[i])
				continue;

			g_print ("\tIL0x%x[0x%0x] ->", sp->il_offset, sp->native_offset);
			for (GSList *l = next[i]; l; l = l->next) {
				guint next_index = GPOINTER_TO_UINT (l->data);
				g_print (" IL0x%x", seq_points[next_index]->il_offset);
			}
			g_print ("\n");
		}
	}

	info = mono_seq_point_info_new (array->len, TRUE, array->data, TRUE, &seq_info_size);
	mono_atomic_fetch_add_i32 (&mono_jit_stats.allocated_seq_points_size, seq_info_size);

	g_byte_array_free (array, TRUE);

	jinfo->seq_points = info;
}

} // namespace mono::interp
