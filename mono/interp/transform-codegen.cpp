/**
 * \file
 * \brief Writing the interpreter IR out as the bytecode the engine runs.
 *
 * Every instruction gets its final length, the blocks get their offsets, and
 * the branches that pointed at blocks get the offsets patched in.
 */

#include "config.h"

#include <mono/metadata/class-internals.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/seq-points-data.h>

#include <mono/mini/mini-runtime.h>

#include "mintops.hpp"
#include "interp-internals.hpp"
#include "transform.hpp"
#include "transform-internal.hpp"

namespace mono::interp {

void
TransformData::handle_relocations ()
{
	// Handle relocations
	for (Reloc *reloc : relocs) {
		int offset = reloc->target_bb->native_offset - reloc->offset;

		switch (reloc->type) {
		case RELOC_SHORT_BRANCH:
			g_assert (new_code [reloc->offset + reloc->skip + 1] == 0xdead);
			new_code [reloc->offset + reloc->skip + 1] = offset;
			break;
		case RELOC_LONG_BRANCH: {
			guint16 *v = (guint16 *) &offset;
			g_assert (new_code [reloc->offset + reloc->skip + 1] == 0xdead);
			g_assert (new_code [reloc->offset + reloc->skip + 2] == 0xbeef);
			new_code [reloc->offset + reloc->skip + 1] = *(guint16 *) v;
			new_code [reloc->offset + reloc->skip + 2] = *(guint16 *) (v + 1);
			break;
		}
		case RELOC_SWITCH: {
			guint16 *v = (guint16*)&offset;
			g_assert (new_code [reloc->offset] == 0xdead);
			g_assert (new_code [reloc->offset + 1] == 0xbeef);
			new_code [reloc->offset] = *(guint16*)v;
			new_code [reloc->offset + 1] = *(guint16*)(v + 1);
			break;
		}
		default:
			g_assert_not_reached ();
			break;
		}
	}
}

static int
get_inst_length (InterpInst *ins)
{
	if (ins->opcode == MINT_SWITCH)
		return MINT_SWITCH_LEN (READ32 (&ins->data [0]));
	else
		return oplen (ins->opcode);
}


guint16*
TransformData::emit_compacted_instruction (guint16* start_ip, InterpBasicBlock *bb,
                            InterpInst *ins)
{
	guint16 opcode = ins->opcode;
	guint16 *ip = start_ip;

	// We know what IL offset this instruction was created for. We can now map the IL offset
	// to the IR offset. We use this array to resolve the relocations, which reference the IL.
	if (ins->il_offset != -1 && !in_offsets [ins->il_offset]) {
		g_assert (ins->il_offset >= 0 && ins->il_offset < header->code_size);
		in_offsets [ins->il_offset] = start_ip - new_code + 1;

		MonoDebugLineNumberEntry lne;
		lne.native_offset = (guint8*)start_ip - (guint8*)new_code;
		lne.il_offset = ins->il_offset;
		line_numbers.push_back (lne);
	}

	if (opcode == MINT_NOP)
		return ip;

	*ip++ = opcode;
	if (opcode == MINT_SWITCH) {
		int labels = READ32 (&ins->data [0]);
		*ip++ = get_interp_local_offset (ins->sregs [0], TRUE);
		// Write number of switch labels
		*ip++ = ins->data [0];
		*ip++ = ins->data [1];
		// Add relocation for each label
		for (int i = 0; i < labels; i++) {
			Reloc *reloc = arena.create<Reloc> ();
			reloc->type = RELOC_SWITCH;
			reloc->offset = ip - new_code;
			reloc->target_bb = ins->info.target_bb_table [i];
			relocs.push_back (reloc);
			*ip++ = 0xdead;
			*ip++ = 0xbeef;
		}
	} else if ((opcode >= MINT_BRFALSE_I4_S && opcode <= MINT_BRTRUE_R8_S) ||
			(opcode >= MINT_BEQ_I4_S && opcode <= MINT_BLT_UN_R8_S) ||
			opcode == MINT_BR_S || opcode == MINT_LEAVE_S || opcode == MINT_LEAVE_S_CHECK || opcode == MINT_CALL_HANDLER_S) {
		const int br_offset = start_ip - new_code;
		for (int i = 0; i < num_sregs (opcode); i++)
			*ip++ = get_interp_local_offset (ins->sregs [i], TRUE);
		if (ins->info.target_bb->native_offset >= 0) {
			// Backwards branch. We can already patch it.
			*ip++ = ins->info.target_bb->native_offset - br_offset;
		} else {
			// We don't know the in_offset of the target, add a reloc
			Reloc *reloc = arena.create<Reloc> ();
			reloc->type = RELOC_SHORT_BRANCH;
			reloc->skip = num_sregs (opcode);
			reloc->offset = br_offset;
			reloc->target_bb = ins->info.target_bb;
			relocs.push_back (reloc);
			*ip++ = 0xdead;
		}
		if (opcode == MINT_CALL_HANDLER_S)
			*ip++ = ins->data [1];
	} else if ((opcode >= MINT_BRFALSE_I4 && opcode <= MINT_BRTRUE_R8) ||
			(opcode >= MINT_BEQ_I4 && opcode <= MINT_BLT_UN_R8) ||
			opcode == MINT_BR || opcode == MINT_LEAVE || opcode == MINT_LEAVE_CHECK || opcode == MINT_CALL_HANDLER) {
		const int br_offset = start_ip - new_code;
		for (int i = 0; i < num_sregs (opcode); i++)
			*ip++ = get_interp_local_offset (ins->sregs [i], TRUE);
		if (ins->info.target_bb->native_offset >= 0) {
			// Backwards branch. We can already patch it
			int target_offset = ins->info.target_bb->native_offset - br_offset;
			WRITE32 (ip, &target_offset);
		} else {
			Reloc *reloc = arena.create<Reloc> ();
			reloc->type = RELOC_LONG_BRANCH;
			reloc->skip = num_sregs (opcode);
			reloc->offset = br_offset;
			reloc->target_bb = ins->info.target_bb;
			relocs.push_back (reloc);
			*ip++ = 0xdead;
			*ip++ = 0xbeef;
		}
		if (opcode == MINT_CALL_HANDLER)
			*ip++ = ins->data [2];
	} else if (opcode == MINT_SDB_SEQ_POINT) {
		SeqPoint *seqp = arena.create<SeqPoint> ();

		if (ins->flags & INTERP_INST_FLAG_SEQ_POINT_METHOD_ENTRY)
			seqp->il_offset = METHOD_ENTRY_IL_OFFSET;
		else if (ins->flags & INTERP_INST_FLAG_SEQ_POINT_METHOD_EXIT)
			seqp->il_offset = METHOD_EXIT_IL_OFFSET;
		else
			seqp->il_offset = ins->il_offset;

		seqp->native_offset = (guint8*)start_ip - (guint8*)new_code;
		if (ins->flags & INTERP_INST_FLAG_SEQ_POINT_NONEMPTY_STACK)
			seqp->flags |= MONO_SEQ_POINT_FLAG_NONEMPTY_STACK;
		if (ins->flags & INTERP_INST_FLAG_SEQ_POINT_NESTED_CALL)
			seqp->flags |= MONO_SEQ_POINT_FLAG_NESTED_CALL;
		seq_points.push_back (seqp);

		/*
		 * The block the instruction sits in, rather than the one that starts at
		 * its IL offset. A sequence point comes from the symbol file and lands
		 * wherever the line does, which is usually not a block boundary, and
		 * offset_to_bb holds an entry only where a block begins.
		 */
		bb->seq_points = g_slist_prepend_mempool (arena.pool (), bb->seq_points, seqp);
		bb->last_seq_point = seqp;
	} else {
		if (num_dregs (opcode))
			*ip++ = get_interp_local_offset (ins->dreg, TRUE);

		if (num_sregs (opcode)) {
			for (int i = 0; i < num_sregs (opcode); i++)
				*ip++ = get_interp_local_offset (ins->sregs [i], TRUE);
		} else if (opcode == MINT_LDLOCA_S) {
			// This opcode receives a local but it is not viewed as a sreg since we don't load the value
			*ip++ = get_interp_local_offset (ins->sregs [0], TRUE);
		}

		int left = get_inst_length (ins) - (ip - start_ip);
		// Emit the rest of the data
		for (int i = 0; i < left; i++)
			*ip++ = ins->data [i];
	}
	mono_interp_stats.emitted_instructions++;
	return ip;
}

void
TransformData::alloc_ins_locals (InterpInst *ins)
{
	int opcode = ins->opcode;
	if (num_sregs (opcode)) {
		for (int i = 0; i < num_sregs (opcode); i++)
			get_interp_local_offset (ins->sregs [i], FALSE);
	} else if (opcode == MINT_LDLOCA_S) {
		// This opcode receives a local but it is not viewed as a sreg since we don't load the value
		get_interp_local_offset (ins->sregs [0], FALSE);
	}

	if (num_dregs (opcode))
		get_interp_local_offset (ins->dreg, FALSE);
}

// Generates the final code, after we are done with all the passes
void
TransformData::generate_compacted_code ()
{
	guint16 *ip;
	int size = 0;

	// Iterate once for preliminary computations
	for (InterpBasicBlock *bb : blocks_from (entry_bb)) {
		for (InterpInst *ins : *bb) {
			size += get_inst_length (ins);
			alloc_ins_locals (ins);
		}
	}

	// Generate the compacted stream of instructions
	new_code = ip = (guint16*)mono_mem_manager_alloc0 (mem_manager, size * sizeof (guint16));

	for (InterpBasicBlock *bb : blocks_from (entry_bb)) {
		bb->native_offset = ip - new_code;
		for (InterpInst *ins : *bb)
			ip = emit_compacted_instruction (ip, bb, ins);
	}
	new_code_end = ip;
	in_offsets [header->code_size] = new_code_end - new_code;

	// Patch all branches. This might be useless since we iterate once anyway to compute the size
	// of the generated code. We could compute the native offset of each basic block then.
	handle_relocations ();
}

} // namespace mono::interp
