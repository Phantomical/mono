/**
 * \file
 * \brief The passes that run over the interpreter IR before it is emitted.
 *
 * Constant propagation and folding, dead local elimination, and the block
 * merging that follows from both.
 */

#include "config.h"

#include <mono/metadata/class-internals.h>
#include <mono/metadata/tabledefs.h>
#include <mono/utils/unlocked.h>

#include <mono/mini/mini-runtime.h>

#include "mintops.hpp"
#include "runtime/internals.hpp"
#include "transform.hpp"
#include "internal.hpp"

namespace mono::interp {

// Traverse the list of basic blocks and merge adjacent blocks
gboolean
TransformData::interp_optimize_bblocks ()
{
	InterpBasicBlock *bb = entry_bb;
	gboolean needs_cprop = FALSE;

	while (TRUE) {
		InterpBasicBlock *next_bb = bb->next_bb;
		if (!next_bb)
			break;
		if (next_bb->in_count == 0 && !next_bb->eh_block) {
			if (verbose_level)
				g_print ("Removed BB%d\n", next_bb->index);
			interp_remove_bblock (next_bb, bb);
			continue;
		} else if (bb->out_count == 1 && bb->out_bb [0] == next_bb && next_bb->in_count == 1 && !next_bb->eh_block) {
			g_assert (next_bb->in_bb [0] == bb);
			interp_merge_bblocks (bb, next_bb);
			if (verbose_level)
				g_print ("Merged BB%d and BB%d\n", bb->index, next_bb->index);
			needs_cprop = TRUE;
			continue;
		}

		bb = next_bb;
	}
	return needs_cprop;
}

gboolean
TransformData::interp_local_deadce (int *local_ref_count)
{
	gboolean needs_dce = FALSE;
	gboolean needs_cprop = FALSE;

	for (size_t i = 0; i < locals.size (); i++) {
		g_assert (local_ref_count [i] >= 0);
		g_assert (locals [i].indirects >= 0);
		if (!local_ref_count [i] &&
				!locals [i].indirects &&
				!(locals [i].flags & INTERP_LOCAL_FLAG_CALL_ARGS) &&
				(locals [i].flags & INTERP_LOCAL_FLAG_DEAD) == 0) {
			needs_dce = TRUE;
			locals [i].flags |= INTERP_LOCAL_FLAG_DEAD;
		}
	}

	// Return early if all locals are alive
	if (!needs_dce)
		return FALSE;

	// Kill instructions that don't use stack and are storing into dead locals
	for (InterpBasicBlock *bb : blocks_from (entry_bb)) {
		for (InterpInst *ins : *bb) {
			if (MINT_IS_MOV (ins->opcode) ||
					MINT_IS_LDC_I4 (ins->opcode) ||
					ins->opcode == MINT_LDC_I8 ||
					ins->opcode == MINT_MONO_LDPTR ||
					ins->opcode == MINT_LDLOCA_S) {
				int dreg = ins->dreg;
				if (locals [dreg].flags & INTERP_LOCAL_FLAG_DEAD) {
					if (verbose_level) {
						g_print ("kill dead ins:\n\t");
						dump_interp_inst (ins);
					}

					if (ins->opcode == MINT_LDLOCA_S) {
						mono_interp_stats.ldlocas_removed++;
						locals [ins->sregs [0]].indirects--;
						if (!locals [ins->sregs [0]].indirects) {
							// We can do cprop now through this local. Run cprop again.
							needs_cprop = TRUE;
						}
					}
					interp_clear_ins (ins);
					mono_interp_stats.killed_instructions++;
					// FIXME This is lazy. We should update the ref count for the sregs and redo deadce.
					needs_cprop = TRUE;
				}
			}
		}
	}
	return needs_cprop;
}

/*
 * The folding macros below all start by checking that the constant they found
 * has the type the opcode reads.  It does not always: invalid IL can merge an
 * int32 and an int64 into one stack slot, and the opcode then names a width the
 * constant does not have.  Declining to fold leaves the instruction to run, so
 * a bad program gets whatever the handler computes rather than an abort.
 */
#define INTERP_FOLD_UNOP(opcode,val_type,field,op) \
	case opcode: \
		if (val->type != val_type) return ins; \
		result.type = val_type; \
		result.field = op val->field; \
		break;

#define INTERP_FOLD_CONV(opcode,val_type_dst,field_dst,val_type_src,field_src,cast_type) \
	case opcode: \
		if (val->type != val_type_src) return ins; \
		result.type = val_type_dst; \
		result.field_dst = (cast_type)val->field_src; \
		break;

#define INTERP_FOLD_CONV_FULL(opcode,val_type_dst,field_dst,val_type_src,field_src,cast_type,cond) \
	case opcode: \
		if (val->type != val_type_src) return ins; \
		if (!(cond)) return ins; \
		result.type = val_type_dst; \
		result.field_dst = (cast_type)val->field_src; \
		break;

InterpInst*
TransformData::interp_fold_unop (LocalValue *local_defs, int *local_ref_count, InterpInst *ins)
{
	// ins should be an unop, therefore it should have a single dreg and a single sreg
	int dreg = ins->dreg;
	int sreg = ins->sregs [0];
	LocalValue *val = &local_defs [sreg];
	LocalValue result;

	if (val->type != LOCAL_VALUE_I4 && val->type != LOCAL_VALUE_I8)
		return ins;

	// Top of the stack is a constant
	switch (ins->opcode) {
		INTERP_FOLD_UNOP (MINT_ADD1_I4, LOCAL_VALUE_I4, i, 1+);
		INTERP_FOLD_UNOP (MINT_ADD1_I8, LOCAL_VALUE_I8, l, 1+);
		INTERP_FOLD_UNOP (MINT_SUB1_I4, LOCAL_VALUE_I4, i, -1+);
		INTERP_FOLD_UNOP (MINT_SUB1_I8, LOCAL_VALUE_I8, l, -1+);
		INTERP_FOLD_UNOP (MINT_NEG_I4, LOCAL_VALUE_I4, i, -);
		INTERP_FOLD_UNOP (MINT_NEG_I8, LOCAL_VALUE_I8, l, -);
		INTERP_FOLD_UNOP (MINT_NOT_I4, LOCAL_VALUE_I4, i, ~);
		INTERP_FOLD_UNOP (MINT_NOT_I8, LOCAL_VALUE_I8, l, ~);
		INTERP_FOLD_UNOP (MINT_CEQ0_I4, LOCAL_VALUE_I4, i, 0 ==);

		// MOV's are just a copy, if the contents of sreg are known
		INTERP_FOLD_CONV (MINT_MOV_I1, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, gint32);
		INTERP_FOLD_CONV (MINT_MOV_U1, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, gint32);
		INTERP_FOLD_CONV (MINT_MOV_I2, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, gint32);
		INTERP_FOLD_CONV (MINT_MOV_U2, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, gint32);

		INTERP_FOLD_CONV (MINT_CONV_I1_I4, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, gint8);
		INTERP_FOLD_CONV (MINT_CONV_I1_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, gint8);
		INTERP_FOLD_CONV (MINT_CONV_U1_I4, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, guint8);
		INTERP_FOLD_CONV (MINT_CONV_U1_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, guint8);

		INTERP_FOLD_CONV (MINT_CONV_I2_I4, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, gint16);
		INTERP_FOLD_CONV (MINT_CONV_I2_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, gint16);
		INTERP_FOLD_CONV (MINT_CONV_U2_I4, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, guint16);
		INTERP_FOLD_CONV (MINT_CONV_U2_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, guint16);

		INTERP_FOLD_CONV (MINT_CONV_I4_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, gint32);
		INTERP_FOLD_CONV (MINT_CONV_U4_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, gint32);

		/* An index that does not fit keeps its instruction, which is what fails the bound test. */
		INTERP_FOLD_CONV_FULL (MINT_CONV_INDEX_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, gint32, (guint64) val->l <= G_MAXINT32);

		INTERP_FOLD_CONV (MINT_CONV_I8_I4, LOCAL_VALUE_I8, l, LOCAL_VALUE_I4, i, gint32);
		INTERP_FOLD_CONV (MINT_CONV_I8_U4, LOCAL_VALUE_I8, l, LOCAL_VALUE_I4, i, guint32);

		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_I1_I4, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, gint8, val->i >= G_MININT8 && val->i <= G_MAXINT8);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_I1_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, gint8, val->l >= G_MININT8 && val->l <= G_MAXINT8);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_I1_U4, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, gint8, val->i >= 0 && val->i <= G_MAXINT8);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_I1_U8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, gint8, val->l >= 0 && val->l <= G_MAXINT8);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_U1_I4, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, guint8, val->i >= 0 && val->i <= G_MAXUINT8);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_U1_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, guint8, val->l >= 0 && val->l <= G_MAXUINT8);

		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_I2_I4, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, gint16, val->i >= G_MININT16 && val->i <= G_MAXINT16);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_I2_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, i, gint16, val->l >= G_MININT16 && val->l <= G_MAXINT16);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_I2_U4, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, gint16, val->i >= 0 && val->i <= G_MAXINT16);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_I2_U8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, gint16, val->l >= 0 && val->l <= G_MAXINT16);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_U2_I4, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, guint16, val->i >= 0 && val->i <= G_MAXUINT16);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_U2_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, guint16, val->l >= 0 && val->l <= G_MAXUINT16);

		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_I4_U4, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, gint32, val->i >= 0);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_I4_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, gint32, val->l >= G_MININT32 && val->l <= G_MAXINT32);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_I4_U8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, gint32, val->l >= 0 && val->l <= G_MAXINT32);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_U4_I4, LOCAL_VALUE_I4, i, LOCAL_VALUE_I4, i, guint32, val->i >= 0);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_U4_I8, LOCAL_VALUE_I4, i, LOCAL_VALUE_I8, l, guint32, val->l >= 0 && val->l <= G_MAXINT32);

		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_I8_U8, LOCAL_VALUE_I8, l, LOCAL_VALUE_I8, l, gint64, val->l >= 0);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_U8_I4, LOCAL_VALUE_I8, l, LOCAL_VALUE_I4, i, guint64, val->i >= 0);
		INTERP_FOLD_CONV_FULL (MINT_CONV_OVF_U8_I8, LOCAL_VALUE_I8, l, LOCAL_VALUE_I8, l, guint64, val->l >= 0);

		default:
			return ins;
	}

	// We were able to compute the result of the ins instruction. We replace the unop
	// with a LDC of the constant. We leave alone the sregs of this instruction, for
	// deadce to kill the instructions initializing them.
	mono_interp_stats.constant_folds++;

	if (result.type == LOCAL_VALUE_I4)
		ins = interp_get_ldc_i4_from_const (ins, result.i, dreg);
	else if (result.type == LOCAL_VALUE_I8)
		ins = interp_inst_replace_with_i8_const (ins, result.l);
	else
		g_assert_not_reached ();

	if (verbose_level) {
		g_print ("Fold unop :\n\t");
		dump_interp_inst (ins);
	}

	local_ref_count [sreg]--;
	local_defs [dreg] = result;

	return ins;
}

#define INTERP_FOLD_UNOP_BR(_opcode,_local_type,_cond) \
	case _opcode: \
		if (val->type != _local_type) return ins; \
		if (_cond) { \
			ins->opcode = MINT_BR_S; \
			if (cbb->next_bb != ins->info.target_bb) \
				interp_unlink_bblocks (cbb, cbb->next_bb); \
			for (InterpInst *it : instructions_from (ins->next)) \
				interp_clear_ins (it); \
		} else { \
			interp_clear_ins (ins); \
			interp_unlink_bblocks (cbb, ins->info.target_bb); \
		} \
		break;

InterpInst*
TransformData::interp_fold_unop_cond_br (InterpBasicBlock *cbb, LocalValue *local_defs, int *local_ref_count, InterpInst *ins)
{
	// ins should be an unop conditional branch, therefore it should have a single sreg
	int sreg = ins->sregs [0];
	LocalValue *val = &local_defs [sreg];

	if (val->type != LOCAL_VALUE_I4 && val->type != LOCAL_VALUE_I8)
		return ins;

	// Top of the stack is a constant
	switch (ins->opcode) {
		INTERP_FOLD_UNOP_BR (MINT_BRFALSE_I4_S, LOCAL_VALUE_I4, val->i == 0);
		INTERP_FOLD_UNOP_BR (MINT_BRFALSE_I8_S, LOCAL_VALUE_I8, val->l == 0);
		INTERP_FOLD_UNOP_BR (MINT_BRTRUE_I4_S, LOCAL_VALUE_I4, val->i != 0);
		INTERP_FOLD_UNOP_BR (MINT_BRTRUE_I8_S, LOCAL_VALUE_I8, val->l != 0);

		default:
			return ins;
	}

	if (verbose_level) {
		g_print ("Fold unop cond br :\n\t");
		dump_interp_inst (ins);
	}

	mono_interp_stats.constant_folds++;
	local_ref_count [sreg]--;
	return ins;
}

#define INTERP_FOLD_BINOP(opcode,local_type,field,op) \
	case opcode: \
		if (val1->type != local_type || val2->type != local_type) return ins; \
		result.type = local_type; \
		result.field = val1->field op val2->field; \
		break;

#define INTERP_FOLD_BINOP_FULL(opcode,local_type,field,op,cast_type,cond) \
	case opcode: \
		if (val1->type != local_type || val2->type != local_type) return ins; \
		if (!(cond)) return ins; \
		result.type = local_type; \
		result.field = (cast_type)val1->field op (cast_type)val2->field; \
		break;

#define INTERP_FOLD_SHIFTOP(opcode,local_type,field,shift_op,cast_type) \
	case opcode: \
		if (val2->type != LOCAL_VALUE_I4) return ins; \
		result.type = local_type; \
		result.field = (cast_type)val1->field shift_op val2->i; \
		break;

#define INTERP_FOLD_RELOP(opcode,local_type,field,relop,cast_type) \
	case opcode: \
		if (val1->type != local_type || val2->type != local_type) return ins; \
		result.type = LOCAL_VALUE_I4; \
		result.i = (cast_type) val1->field relop (cast_type) val2->field; \
		break;


InterpInst*
TransformData::interp_fold_binop (LocalValue *local_defs, int *local_ref_count, InterpInst *ins)
{
	// ins should be a binop, therefore it should have a single dreg and two sregs
	int dreg = ins->dreg;
	int sreg1 = ins->sregs [0];
	int sreg2 = ins->sregs [1];
	LocalValue *val1 = &local_defs [sreg1];
	LocalValue *val2 = &local_defs [sreg2];
	LocalValue result;

	if (val1->type != LOCAL_VALUE_I4 && val1->type != LOCAL_VALUE_I8)
		return ins;
	if (val2->type != LOCAL_VALUE_I4 && val2->type != LOCAL_VALUE_I8)
		return ins;

	// Top two values of the stack are constants
	switch (ins->opcode) {
		INTERP_FOLD_BINOP (MINT_ADD_I4, LOCAL_VALUE_I4, i, +);
		INTERP_FOLD_BINOP (MINT_ADD_I8, LOCAL_VALUE_I8, l, +);
		INTERP_FOLD_BINOP (MINT_SUB_I4, LOCAL_VALUE_I4, i, -);
		INTERP_FOLD_BINOP (MINT_SUB_I8, LOCAL_VALUE_I8, l, -);
		INTERP_FOLD_BINOP (MINT_MUL_I4, LOCAL_VALUE_I4, i, *);
		INTERP_FOLD_BINOP (MINT_MUL_I8, LOCAL_VALUE_I8, l, *);

		INTERP_FOLD_BINOP (MINT_AND_I4, LOCAL_VALUE_I4, i, &);
		INTERP_FOLD_BINOP (MINT_AND_I8, LOCAL_VALUE_I8, l, &);
		INTERP_FOLD_BINOP (MINT_OR_I4, LOCAL_VALUE_I4, i, |);
		INTERP_FOLD_BINOP (MINT_OR_I8, LOCAL_VALUE_I8, l, |);
		INTERP_FOLD_BINOP (MINT_XOR_I4, LOCAL_VALUE_I4, i, ^);
		INTERP_FOLD_BINOP (MINT_XOR_I8, LOCAL_VALUE_I8, l, ^);

		INTERP_FOLD_SHIFTOP (MINT_SHL_I4, LOCAL_VALUE_I4, i, <<, gint32);
		INTERP_FOLD_SHIFTOP (MINT_SHL_I8, LOCAL_VALUE_I8, l, <<, gint64);
		INTERP_FOLD_SHIFTOP (MINT_SHR_I4, LOCAL_VALUE_I4, i, >>, gint32);
		INTERP_FOLD_SHIFTOP (MINT_SHR_I8, LOCAL_VALUE_I8, l, >>, gint64);
		INTERP_FOLD_SHIFTOP (MINT_SHR_UN_I4, LOCAL_VALUE_I4, i, >>, guint32);
		INTERP_FOLD_SHIFTOP (MINT_SHR_UN_I8, LOCAL_VALUE_I8, l, >>, guint64);

		INTERP_FOLD_RELOP (MINT_CEQ_I4, LOCAL_VALUE_I4, i, ==, gint32);
		INTERP_FOLD_RELOP (MINT_CEQ_I8, LOCAL_VALUE_I8, l, ==, gint64);
		INTERP_FOLD_RELOP (MINT_CNE_I4, LOCAL_VALUE_I4, i, !=, gint32);
		INTERP_FOLD_RELOP (MINT_CNE_I8, LOCAL_VALUE_I8, l, !=, gint64);

		INTERP_FOLD_RELOP (MINT_CGT_I4, LOCAL_VALUE_I4, i, >, gint32);
		INTERP_FOLD_RELOP (MINT_CGT_I8, LOCAL_VALUE_I8, l, >, gint64);
		INTERP_FOLD_RELOP (MINT_CGT_UN_I4, LOCAL_VALUE_I4, i, >, guint32);
		INTERP_FOLD_RELOP (MINT_CGT_UN_I8, LOCAL_VALUE_I8, l, >, guint64);

		INTERP_FOLD_RELOP (MINT_CGE_I4, LOCAL_VALUE_I4, i, >=, gint32);
		INTERP_FOLD_RELOP (MINT_CGE_I8, LOCAL_VALUE_I8, l, >=, gint64);
		INTERP_FOLD_RELOP (MINT_CGE_UN_I4, LOCAL_VALUE_I4, i, >=, guint32);
		INTERP_FOLD_RELOP (MINT_CGE_UN_I8, LOCAL_VALUE_I8, l, >=, guint64);

		INTERP_FOLD_RELOP (MINT_CLT_I4, LOCAL_VALUE_I4, i, <, gint32);
		INTERP_FOLD_RELOP (MINT_CLT_I8, LOCAL_VALUE_I8, l, <, gint64);
		INTERP_FOLD_RELOP (MINT_CLT_UN_I4, LOCAL_VALUE_I4, i, <, guint32);
		INTERP_FOLD_RELOP (MINT_CLT_UN_I8, LOCAL_VALUE_I8, l, <, guint64);

		INTERP_FOLD_RELOP (MINT_CLE_I4, LOCAL_VALUE_I4, i, <=, gint32);
		INTERP_FOLD_RELOP (MINT_CLE_I8, LOCAL_VALUE_I8, l, <=, gint64);
		INTERP_FOLD_RELOP (MINT_CLE_UN_I4, LOCAL_VALUE_I4, i, <=, guint32);
		INTERP_FOLD_RELOP (MINT_CLE_UN_I8, LOCAL_VALUE_I8, l, <=, guint64);

		INTERP_FOLD_BINOP_FULL (MINT_DIV_I4, LOCAL_VALUE_I4, i, /, gint32, val2->i != 0 && (val1->i != G_MININT32 || val2->i != -1));
		INTERP_FOLD_BINOP_FULL (MINT_DIV_I8, LOCAL_VALUE_I8, l, /, gint64, val2->l != 0 && (val1->l != G_MININT64 || val2->l != -1));
		INTERP_FOLD_BINOP_FULL (MINT_DIV_UN_I4, LOCAL_VALUE_I4, i, /, guint32, val2->i != 0);
		INTERP_FOLD_BINOP_FULL (MINT_DIV_UN_I8, LOCAL_VALUE_I8, l, /, guint64, val2->l != 0);

		INTERP_FOLD_BINOP_FULL (MINT_REM_I4, LOCAL_VALUE_I4, i, %, gint32, val2->i != 0 && (val1->i != G_MININT32 || val2->i != -1));
		INTERP_FOLD_BINOP_FULL (MINT_REM_I8, LOCAL_VALUE_I8, l, %, gint64, val2->l != 0 && (val1->l != G_MININT64 || val2->l != -1));
		INTERP_FOLD_BINOP_FULL (MINT_REM_UN_I4, LOCAL_VALUE_I4, i, %, guint32, val2->i != 0);
		INTERP_FOLD_BINOP_FULL (MINT_REM_UN_I8, LOCAL_VALUE_I8, l, %, guint64, val2->l != 0);

		default:
			return ins;
	}

	// We were able to compute the result of the ins instruction. We replace the binop
	// with a LDC of the constant. We leave alone the sregs of this instruction, for
	// deadce to kill the instructions initializing them.
	mono_interp_stats.constant_folds++;

	if (result.type == LOCAL_VALUE_I4)
		ins = interp_get_ldc_i4_from_const (ins, result.i, dreg);
	else if (result.type == LOCAL_VALUE_I8)
		ins = interp_inst_replace_with_i8_const (ins, result.l);
	else
		g_assert_not_reached ();

	if (verbose_level) {
		g_print ("Fold binop :\n\t");
		dump_interp_inst (ins);
	}

	local_ref_count [sreg1]--;
	local_ref_count [sreg2]--;
	local_defs [dreg] = result;
	return ins;
}

// Due to poor current design, the branch op might not be the last instruction in the bblock
// (in case we fallthrough and need to have the stack locals match the ones from next_bb, done
// in fixup_newbb_stack_locals). If that's the case, clear all these mov's. This helps bblock
// merging quickly find the MINT_BR_S opcode.
#define INTERP_FOLD_BINOP_BR(_opcode,_local_type,_cond) \
	case _opcode: \
		if (val1->type != _local_type || val2->type != _local_type) return ins; \
		if (_cond) { \
			ins->opcode = MINT_BR_S; \
			if (cbb->next_bb != ins->info.target_bb) \
				interp_unlink_bblocks (cbb, cbb->next_bb); \
			for (InterpInst *it : instructions_from (ins->next)) \
				interp_clear_ins (it); \
		} else { \
			interp_clear_ins (ins); \
			interp_unlink_bblocks (cbb, ins->info.target_bb); \
		} \
		break;

InterpInst*
TransformData::interp_fold_binop_cond_br (InterpBasicBlock *cbb, LocalValue *local_defs, int *local_ref_count, InterpInst *ins)
{
	// ins should be a conditional binop, therefore it should have only two sregs
	int sreg1 = ins->sregs [0];
	int sreg2 = ins->sregs [1];
	LocalValue *val1 = &local_defs [sreg1];
	LocalValue *val2 = &local_defs [sreg2];

	if (val1->type != LOCAL_VALUE_I4 && val1->type != LOCAL_VALUE_I8)
		return ins;
	if (val2->type != LOCAL_VALUE_I4 && val2->type != LOCAL_VALUE_I8)
		return ins;

	switch (ins->opcode) {
		INTERP_FOLD_BINOP_BR (MINT_BEQ_I4_S, LOCAL_VALUE_I4, val1->i == val2->i);
		INTERP_FOLD_BINOP_BR (MINT_BEQ_I8_S, LOCAL_VALUE_I8, val1->l == val2->l);
		INTERP_FOLD_BINOP_BR (MINT_BGE_I4_S, LOCAL_VALUE_I4, val1->i >= val2->i);
		INTERP_FOLD_BINOP_BR (MINT_BGE_I8_S, LOCAL_VALUE_I8, val1->l >= val2->l);
		INTERP_FOLD_BINOP_BR (MINT_BGT_I4_S, LOCAL_VALUE_I4, val1->i > val2->i);
		INTERP_FOLD_BINOP_BR (MINT_BGT_I8_S, LOCAL_VALUE_I8, val1->l > val2->l);
		INTERP_FOLD_BINOP_BR (MINT_BLT_I4_S, LOCAL_VALUE_I4, val1->i < val2->i);
		INTERP_FOLD_BINOP_BR (MINT_BLT_I8_S, LOCAL_VALUE_I8, val1->l < val2->l);
		INTERP_FOLD_BINOP_BR (MINT_BLE_I4_S, LOCAL_VALUE_I4, val1->i <= val2->i);
		INTERP_FOLD_BINOP_BR (MINT_BLE_I8_S, LOCAL_VALUE_I8, val1->l <= val2->l);

		INTERP_FOLD_BINOP_BR (MINT_BNE_UN_I4_S, LOCAL_VALUE_I4, val1->i != val2->i);
		INTERP_FOLD_BINOP_BR (MINT_BNE_UN_I8_S, LOCAL_VALUE_I8, val1->l != val2->l);
		INTERP_FOLD_BINOP_BR (MINT_BGE_UN_I4_S, LOCAL_VALUE_I4, (guint32)val1->i >= (guint32)val2->i);
		INTERP_FOLD_BINOP_BR (MINT_BGE_UN_I8_S, LOCAL_VALUE_I8, (guint64)val1->l >= (guint64)val2->l);
		INTERP_FOLD_BINOP_BR (MINT_BGT_UN_I4_S, LOCAL_VALUE_I4, (guint32)val1->i > (guint32)val2->i);
		INTERP_FOLD_BINOP_BR (MINT_BGT_UN_I8_S, LOCAL_VALUE_I8, (guint64)val1->l > (guint64)val2->l);
		INTERP_FOLD_BINOP_BR (MINT_BLE_UN_I4_S, LOCAL_VALUE_I4, (guint32)val1->i <= (guint32)val2->i);
		INTERP_FOLD_BINOP_BR (MINT_BLE_UN_I8_S, LOCAL_VALUE_I8, (guint64)val1->l <= (guint64)val2->l);
		INTERP_FOLD_BINOP_BR (MINT_BLT_UN_I4_S, LOCAL_VALUE_I4, (guint32)val1->i < (guint32)val2->i);
		INTERP_FOLD_BINOP_BR (MINT_BLT_UN_I8_S, LOCAL_VALUE_I8, (guint64)val1->l < (guint64)val2->l);

		default:
			return ins;
	}
	if (verbose_level) {
		g_print ("Fold binop cond br :\n\t");
		dump_interp_inst (ins);
	}

	mono_interp_stats.constant_folds++;
	local_ref_count [sreg1]--;
	local_ref_count [sreg2]--;
	return ins;
}

void
TransformData::interp_cprop ()
{
	LocalValue *local_defs = g_new (LocalValue, locals.size ());
	int *local_ref_count = g_new (int, locals.size ());
	InterpBasicBlock *bb;
	gboolean needs_retry;
	int ins_index;

retry:
	memset (local_ref_count, 0, locals.size () * sizeof (int));

	if (verbose_level)
		g_print ("\ncprop iteration\n");

	for (bb = entry_bb; bb != NULL; bb = bb->next_bb) {
		InterpInst *ins;
		ins_index = 0;

		// Set cbb since we do some instruction inserting below
		cbb = bb;

		// FIXME This is excessive. Remove this once we have SSA
		memset (local_defs, 0, locals.size () * sizeof (LocalValue));

		if (verbose_level)
			g_print ("BB%d\n", bb->index);

		for (ins = bb->first_ins; ins != NULL; ins = ins->next) {
			int opcode = ins->opcode;

			if (opcode == MINT_NOP)
				continue;

			int sregs_count = num_sregs (opcode);
			int dregs_count = num_dregs (opcode);
			gint32 *sregs = &ins->sregs [0];
			gint32 dreg = ins->dreg;

			if (verbose_level && ins->opcode != MINT_NOP)
				dump_interp_inst (ins);

			for (int i = 0; i < sregs_count; i++) {
				// FIXME MINT_PROF_EXIT when void
				if (sregs [i] == -1)
					continue;
				local_ref_count [sregs [i]]++;
				if (local_defs [sregs [i]].type == LOCAL_VALUE_LOCAL) {
					int cprop_local = local_defs [sregs [i]].local;
					// We are not allowed to extend the liveness of execution stack locals because
					// it can end up conflicting with another such local. Once we will have our
					// own offset allocator for these locals, this restriction can be lifted.
					if (locals [cprop_local].flags & INTERP_LOCAL_FLAG_EXECUTION_STACK)
						continue;

					// We are trying to replace sregs [i] with its def local (cprop_local), but cprop_local has since been
					// modified, so we can't use it.
					if (local_defs [cprop_local].ins != NULL && local_defs [cprop_local].def_index > local_defs [sregs [i]].def_index)
						continue;

					if (verbose_level)
						g_print ("cprop %d -> %d:\n\t", sregs [i], cprop_local);
					local_ref_count [sregs [i]]--;
					sregs [i] = cprop_local;
					local_ref_count [cprop_local]++;
					if (verbose_level)
						dump_interp_inst (ins);
				}
			}

			if (dregs_count) {
				local_defs [dreg].type = LOCAL_VALUE_NONE;
				local_defs [dreg].ins = ins;
				local_defs [dreg].def_index = ins_index;
			}

			if (opcode == MINT_MOV_4 || opcode == MINT_MOV_8 || opcode == MINT_MOV_VT) {
				int sreg = sregs [0];
				if (dreg == sreg) {
					if (verbose_level)
						g_print ("clear redundant mov\n");
					interp_clear_ins (ins);
					local_ref_count [sreg]--;
				} else if (locals [sreg].indirects || locals [dreg].indirects) {
					// Don't bother with indirect locals
				} else if (local_defs [sreg].type == LOCAL_VALUE_I4 || local_defs [sreg].type == LOCAL_VALUE_I8) {
					// Replace mov with ldc
					gboolean is_i4 = local_defs [sreg].type == LOCAL_VALUE_I4;
					g_assert (!locals [sreg].indirects);
					local_defs [dreg].type = local_defs [sreg].type;
					if (is_i4) {
						int ct = local_defs [sreg].i;
						ins = interp_get_ldc_i4_from_const (ins, ct, dreg);
						local_defs [dreg].i = ct;
					} else {
						gint64 ct = local_defs [sreg].l;
						ins = interp_inst_replace_with_i8_const (ins, ct);
						local_defs [dreg].l = ct;
					}
					local_defs [dreg].ins = ins;
					local_ref_count [sreg]--;
					mono_interp_stats.copy_propagations++;
					if (verbose_level) {
						g_print ("cprop loc %d -> ct :\n\t", sreg);
						dump_interp_inst (ins);
					}
				} else if (local_defs [sreg].ins != NULL &&
						(locals [sreg].flags & INTERP_LOCAL_FLAG_EXECUTION_STACK) &&
						!(locals [sreg].flags & INTERP_LOCAL_FLAG_CALL_ARGS) &&
						!(locals [dreg].flags & INTERP_LOCAL_FLAG_EXECUTION_STACK) &&
						interp_prev_ins (ins) == local_defs [sreg].ins) {
					// hackish temporary optimization that won't be necessary in the future
					// We replace `local1 <- ?, local2 <- local1` with `local2 <- ?, local1 <- local2`
					// if local1 is execution stack local and local2 is normal global local. This makes
					// it more likely for `local1 <- local2` to be killed, while before we always needed
					// to store to the global local, which is likely accessed by other instructions.
					InterpInst *def = local_defs [sreg].ins;
					int original_dreg = def->dreg;

					def->dreg = dreg;
					ins->dreg = original_dreg;
					sregs [0] = dreg;

					local_defs [dreg].type = LOCAL_VALUE_NONE;
					local_defs [dreg].ins = def;
					local_defs [original_dreg].type = LOCAL_VALUE_LOCAL;
					local_defs [original_dreg].ins = ins;
					local_defs [original_dreg].local = dreg;

					local_ref_count [original_dreg]--;
					local_ref_count [dreg]++;

					if (verbose_level) {
						g_print ("cprop dreg:\n\t");
						dump_interp_inst (def);
						g_print ("\t");
						dump_interp_inst (ins);
					}
				} else {
					if (verbose_level)
						g_print ("local copy %d <- %d\n", dreg, sreg);
					local_defs [dreg].type = LOCAL_VALUE_LOCAL;
					local_defs [dreg].local = sreg;
				}
			} else if (opcode == MINT_LDLOCA_S) {
				// The local that we are taking the address of is not a sreg but still referenced
				local_ref_count [ins->sregs [0]]++;
			} else if (MINT_IS_LDC_I4 (opcode)) {
				local_defs [dreg].type = LOCAL_VALUE_I4;
				local_defs [dreg].i = interp_get_const_from_ldc_i4 (ins);
			} else if (opcode == MINT_LDC_I8) {
				local_defs [dreg].type = LOCAL_VALUE_I8;
				local_defs [dreg].l = READ64 (&ins->data [0]);
			} else if (ins->opcode == MINT_MONO_LDPTR) {
#if SIZEOF_VOID_P == 8
				local_defs [dreg].type = LOCAL_VALUE_I8;
				local_defs [dreg].l = (gint64)data_items [ins->data [0]];
#else
				local_defs [dreg].type = LOCAL_VALUE_I4;
				local_defs [dreg].i = (gint32)data_items [ins->data [0]];
#endif
			} else if (MINT_IS_UNOP (opcode) || (opcode >= MINT_MOV_I1 && opcode <= MINT_MOV_U2)) {
				ins = interp_fold_unop (local_defs, local_ref_count, ins);
			} else if (MINT_IS_UNOP_CONDITIONAL_BRANCH (opcode)) {
				ins = interp_fold_unop_cond_br (bb, local_defs, local_ref_count, ins);
			} else if (MINT_IS_BINOP (opcode)) {
				ins = interp_fold_binop (local_defs, local_ref_count, ins);
			} else if (MINT_IS_BINOP_CONDITIONAL_BRANCH (opcode)) {
				ins = interp_fold_binop_cond_br (bb, local_defs, local_ref_count, ins);
			} else if ((ins->opcode == MINT_NEWOBJ_FAST || ins->opcode == MINT_NEWOBJ_VT_FAST) && ins->data [0] == INLINED_METHOD_FLAG) {
				// FIXME Drop the CALL_ARGS flag on the params so this will no longer be necessary
				int param_count = ins->data [3];
				int *newobj_reg_map = ins->info.newobj_reg_map;
				for (int i = 0; i < param_count; i++) {
					int src = newobj_reg_map [2 * i];
					int dst = newobj_reg_map [2 * i + 1];
					local_defs [dst] = local_defs [src];
					local_defs [dst].ins = NULL;
				}
			} else if (MINT_IS_LDFLD (opcode) && ins->data [0] == 0) {
				InterpInst *ldloca = local_defs [sregs [0]].ins;
				MintType mt = mint_type_of_op (MINT_LDFLD_I1, ins->opcode);
				if (ldloca != NULL && ldloca->opcode == MINT_LDLOCA_S &&
						locals [ldloca->sregs [0]].mt == mt) {
					int local = ldloca->sregs [0];
					// Replace LDLOCA + LDFLD with LDLOC, when the loading field represents
					// the entire local. This is the case with loading the only field of an
					// IntPtr. We don't handle value type loads.
					ins->opcode = get_mov_for_type (mt, TRUE);
					// The dreg of the MOV is the same as the dreg of the LDFLD
					local_ref_count [sregs [0]]--;
					sregs [0] = local;

					if (verbose_level) {
						g_print ("Replace ldloca/ldfld pair :\n\t");
						dump_interp_inst (ins->next);
					}
				}
			} else if (MINT_IS_STFLD (opcode) && ins->data [0] == 0) {
				InterpInst *ldloca = local_defs [sregs [0]].ins;
				MintType mt = mint_type_of_op (MINT_STFLD_I1, ins->opcode);
				if (ldloca != NULL && ldloca->opcode == MINT_LDLOCA_S &&
						locals [ldloca->sregs [0]].mt == mt) {
					int local = ldloca->sregs [0];

					ins->opcode = get_mov_for_type (mt, FALSE);
					// The sreg of the MOV is the same as the second sreg of the STFLD
					local_ref_count [sregs [0]]--;
					ins->dreg = local;
					sregs [0] = sregs [1];

					if (verbose_level) {
						g_print ("Replace ldloca/stfld pair (off %p) :\n\t", ldloca->il_offset);
						dump_interp_inst (ins);
					}
				}
			}
			ins_index++;
		}
	}

	needs_retry = interp_local_deadce (local_ref_count);
	if (mono_interp_opt & INTERP_OPT_BBLOCKS)
		needs_retry |= interp_optimize_bblocks ();

	if (needs_retry)
		goto retry;

	g_free (local_defs);
	g_free (local_ref_count);
}


void
TransformData::interp_optimize_code ()
{
	if (mono_interp_opt & INTERP_OPT_BBLOCKS)
		interp_optimize_bblocks ();

	if (mono_interp_opt & INTERP_OPT_CPROP)
		MONO_TIME_TRACK (mono_interp_stats.cprop_time, interp_cprop ());
}

/*
 * Very few methods have localloc. Handle it separately to not impact performance
 * of other methods. We replace the normal return opcodes with opcodes that also
 * reset the localloc stack.
 */
void
TransformData::interp_fix_localloc_ret ()
{
	g_assert (has_localloc);
	for (InterpBasicBlock *bb : blocks_from (entry_bb)) {
		for (InterpInst *ins : *bb) {
			if (ins->opcode >= MINT_RET && ins->opcode <= MINT_RET_VT)
				ins->opcode += MINT_RET_LOCALLOC - MINT_RET;
		}
	}
}

} // namespace mono::interp
