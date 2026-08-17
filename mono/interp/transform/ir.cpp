/**
 * \file
 * \brief The interpreter IR itself: the instruction list and the block graph.
 *
 * Building and editing instructions, linking and unlinking basic blocks, and
 * the walk over the IL that decides where the blocks start.
 */

#include "config.h"

#include <mono/metadata/class-internals.h>
#include <mono/metadata/mono-basic-block.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/opcodes.h>

#include <mono/mini/mini-runtime.h>

#include "mintops.hpp"
#include "interp-internals.hpp"
#include "transform.hpp"
#include "internal.hpp"

namespace mono::interp {

InterpInst*
TransformData::interp_new_ins (guint16 opcode, int len)
{
	InterpInst *new_inst;
	// Size of data region of instruction is length of instruction minus 1 (the opcode slot)
	new_inst = arena.create_flexible<InterpInst> (sizeof (guint16) * ((len > 0) ? (len - 1) : 0));
	new_inst->opcode = opcode;
	new_inst->il_offset = current_il_offset;
	return new_inst;
}

// This version need to be used with switch opcode, which doesn't have constant length
InterpInst*
TransformData::interp_add_ins_explicit (guint16 opcode, int len)
{
	InterpInst *new_inst = interp_new_ins (opcode, len);
	new_inst->prev = cbb->last_ins;
	if (cbb->last_ins)
		cbb->last_ins->next = new_inst;
	else
		cbb->first_ins = new_inst;
	cbb->last_ins = new_inst;
	// We should delete this, but is currently used widely to set the args of an instruction
	last_ins = new_inst;
	return new_inst;
}

InterpInst*
TransformData::interp_add_ins (guint16 opcode)
{
	return interp_add_ins_explicit (opcode, oplen (opcode));
}

InterpInst*
TransformData::interp_insert_ins_bb (InterpBasicBlock *bb, InterpInst *prev_ins, guint16 opcode)
{
	InterpInst *new_inst = interp_new_ins (opcode, oplen (opcode));

	new_inst->prev = prev_ins;

	if (prev_ins) {
		new_inst->next = prev_ins->next;
		prev_ins->next = new_inst;
	} else {
		new_inst->next = bb->first_ins;
		bb->first_ins = new_inst;
	}

	if (new_inst->next == NULL)
		bb->last_ins = new_inst;
	else
		new_inst->next->prev = new_inst;

	return new_inst;
}

/* Inserts a new instruction after prev_ins. prev_ins must be in cbb */
InterpInst*
TransformData::interp_insert_ins (InterpInst *prev_ins, guint16 opcode)
{
	return interp_insert_ins_bb (cbb, prev_ins, opcode);
}

void
interp_clear_ins (InterpInst *ins)
{
	// Clearing instead of removing from the list makes everything easier.
	// We don't change structure of the instruction list, we don't need
	// to worry about updating the il_offset, or whether this instruction
	// was at the start of a basic block etc.
	ins->opcode = MINT_NOP;
}

InterpInst*
interp_prev_ins (InterpInst *ins)
{
	ins = ins->prev;
	while (ins && ins->opcode == MINT_NOP)
		ins = ins->prev;
	return ins;
}

void
TransformData::mark_bb_as_dead (InterpBasicBlock *bb)
{
	// Update IL offset to bb mapping so that offset_to_bb doesn't point to dead
	// bblocks. This mapping can still be needed when computing clause ranges. Since
	// multiple IL offsets can end up pointing to same bblock after optimizations,
	// make sure we update mapping for all of them
	if (bb->ip >= header->code && bb->ip < il_code + header->code_size) {
		// To avoid scanning the entire offset_to_bb array, we scan only in the vicinity
		// of the IL offset of bb. We can stop search when we encounter a different bblock.
		for (int il_offset = bb->ip - il_code; il_offset >= 0; il_offset--) {
			if (offset_to_bb [il_offset] == bb)
				offset_to_bb [il_offset] = bb->next_bb;
			else if (offset_to_bb [il_offset])
				break;
		}
		for (int il_offset = bb->ip - il_code + 1; il_offset < header->code_size; il_offset++) {
			if (offset_to_bb [il_offset] == bb)
				offset_to_bb [il_offset] = bb->next_bb;
			else if (offset_to_bb [il_offset])
				break;
		}
	}

	bb->dead = TRUE;
	// bb should never be used/referenced after this
}

/* Merges two consecutive bbs (in code order) into a single one */
void
TransformData::interp_merge_bblocks (InterpBasicBlock *bb, InterpBasicBlock *bbadd)
{
	g_assert (bbadd->in_count == 1 && bbadd->in_bb [0] == bb);
	g_assert (bb->next_bb == bbadd);

	// Remove the branch instruction to the invalid bblock
	if (bb->last_ins) {
		InterpInst *last_ins = (bb->last_ins->opcode != MINT_NOP) ? bb->last_ins : interp_prev_ins (bb->last_ins);
		if (last_ins) {
			if (last_ins->opcode == MINT_BR || last_ins->opcode == MINT_BR_S) {
				g_assert (last_ins->info.target_bb == bbadd);
				interp_clear_ins (last_ins);
			} else if (last_ins->opcode == MINT_SWITCH) {
				// Weird corner case where empty switch can branch by default to next instruction
				last_ins->opcode = MINT_NOP;
			}
		}
	}

	// Append all instructions from bbadd to bb
	if (bb->last_ins) {
		if (bbadd->first_ins) {
			bb->last_ins->next = bbadd->first_ins;
			bbadd->first_ins->prev = bb->last_ins;
			bb->last_ins = bbadd->last_ins;
		}
	} else {
		bb->first_ins = bbadd->first_ins;
		bb->last_ins = bbadd->last_ins;
	}
	bb->next_bb = bbadd->next_bb;

	// Fixup bb links
	bb->out_count = bbadd->out_count;
	bb->out_bb = bbadd->out_bb;
	for (int i = 0; i < bbadd->out_count; i++) {
		for (int j = 0; j < bbadd->out_bb [i]->in_count; j++) {
			if (bbadd->out_bb [i]->in_bb [j] == bbadd)
				bbadd->out_bb [i]->in_bb [j] = bb;
		}
	}

	mark_bb_as_dead (bbadd);
}

// array must contain ref
static void
remove_bblock_ref (InterpBasicBlock **array, InterpBasicBlock *ref, int len)
{
	int i = 0;
	while (array [i] != ref)
		i++;
	i++;
	while (i < len) {
		array [i - 1] = array [i];
		i++;
	}
}

void
interp_unlink_bblocks (InterpBasicBlock *from, InterpBasicBlock *to)
{
	remove_bblock_ref (from->out_bb, to, from->out_count);
	from->out_count--;
	remove_bblock_ref (to->in_bb, from, to->in_count);
	to->in_count--;
}

void
TransformData::interp_remove_bblock (InterpBasicBlock *bb, InterpBasicBlock *prev_bb)
{
	g_assert (!bb->in_count);
	while (bb->out_count)
		interp_unlink_bblocks (bb, bb->out_bb [0]);
	prev_bb->next_bb = bb->next_bb;
	mark_bb_as_dead (bb);
}

void
TransformData::interp_link_bblocks (InterpBasicBlock *from, InterpBasicBlock *to)
{
	int i;
	gboolean found = FALSE;

	for (i = 0; i < from->out_count; ++i) {
		if (to == from->out_bb [i]) {
			found = TRUE;
			break;
		}
	}
	if (!found) {
		InterpBasicBlock **newa = arena.create_array<InterpBasicBlock *> (from->out_count + 1);
		for (i = 0; i < from->out_count; ++i)
			newa [i] = from->out_bb [i];
		newa [i] = to;
		from->out_count++;
		from->out_bb = newa;
	}

	found = FALSE;
	for (i = 0; i < to->in_count; ++i) {
		if (from == to->in_bb [i]) {
			found = TRUE;
			break;
		}
	}
	if (!found) {
		InterpBasicBlock **newa = arena.create_array<InterpBasicBlock *> (to->in_count + 1);
		for (i = 0; i < to->in_count; ++i)
			newa [i] = to->in_bb [i];
		newa [i] = from;
		to->in_count++;
		to->in_bb = newa;
	}
}

InterpBasicBlock*
TransformData::get_bb (unsigned char *ip, gboolean make_list)
{
	int offset = ip - il_code;
	InterpBasicBlock *bb = offset_to_bb [offset];

	if (!bb) {
		bb = arena.create<InterpBasicBlock> ();
		bb->ip = ip;
		bb->native_offset = -1;
		bb->stack_height = -1;
		bb->index = bb_count++;
		offset_to_bb [offset] = bb;

		if (make_list)
			basic_blocks.push_back (bb);
	}

	return bb;
}

/*
 * get_basic_blocks:
 *
 *   Compute the set of IL level basic blocks.
 */
void
TransformData::get_basic_blocks (MonoMethodHeader *header, gboolean make_list)
{
	guint8 *start = (guint8*)il_code;
	guint8 *end = (guint8*)il_code + code_size;
	guint8 *ip = start;
	unsigned char *target;
	int i;
	guint cli_addr;
	const MonoOpcode *opcode;

	offset_to_bb = arena.create_array<InterpBasicBlock *> (end - start + 1);
	get_bb (start, make_list);

	for (i = 0; i < header->num_clauses; i++) {
		MonoExceptionClause *c = header->clauses + i;
		get_bb (start + c->try_offset, make_list);
		get_bb (start + c->handler_offset, make_list);
		if (c->flags == MONO_EXCEPTION_CLAUSE_FILTER)
			get_bb (start + c->data.filter_offset, make_list);
	}

	while (ip < end) {
		cli_addr = ip - start;
		i = mono_opcode_value ((const guint8 **)&ip, end);
		opcode = &mono_opcodes [i];
		switch (opcode->argument) {
		case MonoInlineNone:
			ip++;
			break;
		case MonoInlineString:
		case MonoInlineType:
		case MonoInlineField:
		case MonoInlineMethod:
		case MonoInlineTok:
		case MonoInlineSig:
		case MonoShortInlineR:
		case MonoInlineI:
			ip += 5;
			break;
		case MonoInlineVar:
			ip += 3;
			break;
		case MonoShortInlineVar:
		case MonoShortInlineI:
			ip += 2;
			break;
		case MonoShortInlineBrTarget:
			target = start + cli_addr + 2 + (signed char)ip [1];
			get_bb (target, make_list);
			ip += 2;
			get_bb (ip, make_list);
			break;
		case MonoInlineBrTarget:
			target = start + cli_addr + 5 + (gint32)read32 (ip + 1);
			get_bb (target, make_list);
			ip += 5;
			get_bb (ip, make_list);
			break;
		case MonoInlineSwitch: {
			guint32 n = read32 (ip + 1);
			guint32 j;
			ip += 5;
			cli_addr += 5 + 4 * n;
			target = start + cli_addr;
			get_bb (target, make_list);

			for (j = 0; j < n; ++j) {
				target = start + cli_addr + (gint32)read32 (ip);
				get_bb (target, make_list);
				ip += 4;
			}
			get_bb (ip, make_list);
			break;
		}
		case MonoInlineR:
		case MonoInlineI8:
			ip += 9;
			break;
		default:
			g_assert_not_reached ();
		}

		if (i == CEE_THROW || i == CEE_ENDFINALLY || i == CEE_RETHROW)
			get_bb (ip, make_list);
	}
}

} // namespace mono::interp
