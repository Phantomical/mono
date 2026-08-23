/**
 * \file
 * \brief The emitters: one CIL concept becoming interpreter instructions.
 *
 * Branches, arithmetic and conversions, indirect and array access, static
 * field access, the throw helpers, and the block set-up an exception clause
 * needs.
 */

#include "config.h"

#include <mono/metadata/abi-details.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/exception-internals.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/metadata-update.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/tabledefs.h>
#include <mono/utils/mono-memory-model.h>

#include <mono/mini/mini.h>
#include <mono/mini/mini-runtime.h>

#include "mintops.hpp"
#include "runtime/internals.hpp"
#include "interp.h"
#include "transform.hpp"
#include "internal.hpp"

namespace mono::interp {

int
get_mov_for_type (MintType mt, gboolean needs_sext)
{
	switch (mt) {
	case MintType::I1:
	case MintType::U1:
	case MintType::I2:
	case MintType::U2:
		if (needs_sext)
			return op_for_mint_type (MINT_MOV_I1, mt);
		else
			return MINT_MOV_4;
	case MintType::I4:
	case MintType::R4:
		return MINT_MOV_4;
	case MintType::I8:
	case MintType::R8:
		return MINT_MOV_8;
	case MintType::O:
#if SIZEOF_VOID_P == 8
		return MINT_MOV_8;
#else
		return MINT_MOV_4;
#endif
	case MintType::VT:
		return MINT_MOV_VT;
	}
	g_assert_not_reached ();
}

/// Call this when the current block branches to newbb, and newbb can carry a
/// stack state.
void
TransformData::fixup_newbb_stack_locals (InterpBasicBlock *newbb)
{
	if (newbb->stack_height <= 0)
		return;

	for (int i = 0; i < newbb->stack_height; i++) {
		/*
		 * Whichever predecessor got here first fixed the floating point width
		 * the block reads its entry stack at, and CIL lets another arrive with
		 * the other one. Convert rather than move the bytes across.
		 */
		coerce_fp (stack + i, newbb->stack_state[i].type);

		int sloc = stack[i].local;
		int dloc = newbb->stack_state[i].local;
		if (sloc != dloc) {
			MintType mt = locals[sloc].mt;
			int mov_op = get_mov_for_type (mt, FALSE);

			// FIXME can be hit in some IL cases. Should we merge the stack states ? (b41002.il)
			// g_assert (mov_op == get_mov_for_type (locals [dloc].mt, FALSE));

			interp_add_ins (mov_op);
			interp_ins_set_sreg (last_ins, stack[i].local);
			interp_ins_set_dreg (last_ins, newbb->stack_state[i].local);

			if (mt == MintType::VT) {
				g_assert (locals[sloc].size == locals[dloc].size);
				last_ins->data[0] = locals[sloc].size;
			}
		}
	}
}

/// Copies the transform's current stack into bb's entry stack state. The first
/// call sets that state, and a later call leaves it alone.
void
TransformData::init_bb_stack_state (InterpBasicBlock *bb)
{
	// FIXME If already initialized, then we need to generate mov to the registers in the state.
	if (bb->stack_height >= 0)
		return;

	bb->stack_height = sp - stack;
	if (bb->stack_height > 0) {
		int size = bb->stack_height * sizeof (stack[0]);
		bb->stack_state = (StackInfo *) arena.alloc (size, alignof (StackInfo));
		memcpy (bb->stack_state, stack, size);
	}
}

void
TransformData::handle_branch (int short_op, int long_op, int offset)
{
	int shorten_branch = 0;
	int target = ip + offset - il_code;
	if (target < 0 || target >= code_size)
		g_assert_not_reached ();
	/* Add exception checkpoint or safepoint for backward branches */
	if (offset < 0) {
		if (mono_threads_are_safepoints_enabled ())
			interp_add_ins (MINT_SAFEPOINT);
		else
			interp_add_ins (MINT_CHECKPOINT);
	}

	InterpBasicBlock *target_bb = offset_to_bb[target];
	g_assert (target_bb);

	if (short_op == MINT_LEAVE_S || short_op == MINT_LEAVE_S_CHECK)
		target_bb->eh_block = TRUE;

	fixup_newbb_stack_locals (target_bb);
	if (offset > 0)
		init_bb_stack_state (target_bb);

	interp_link_bblocks (cbb, target_bb);

	if (header->code_size <= 25000) /* FIX to be precise somehow? */
		shorten_branch = 1;

	if (shorten_branch) {
		interp_add_ins (short_op);
		last_ins->info.target_bb = target_bb;
	} else {
		interp_add_ins (long_op);
		last_ins->info.target_bb = target_bb;
	}
}

void
TransformData::one_arg_branch (int mint_op, int offset, int inst_size)
{
	StackType type =
		sp[-1].type == StackType::O || sp[-1].type == StackType::MP ? StackType::I : sp[-1].type;
	int long_op = op_for_stack_type (mint_op, type);
	int short_op = long_op + MINT_BRFALSE_I4_S - MINT_BRFALSE_I4;
	CHECK_STACK (1);
	--sp;
	if (offset) {
		handle_branch (short_op, long_op, offset + inst_size);
		interp_ins_set_sreg (last_ins, sp->local);
	} else {
		interp_add_ins (MINT_NOP);
	}
}

void
TransformData::interp_add_conv (StackInfo *sp, InterpInst *prev_ins, StackType type, int conv_op)
{
	InterpInst *new_inst;
	if (prev_ins)
		new_inst = interp_insert_ins (prev_ins, conv_op);
	else
		new_inst = interp_add_ins (conv_op);

	interp_ins_set_sreg (new_inst, sp->local);
	set_simple_type_and_local (sp, type);
	interp_ins_set_dreg (new_inst, sp->local);
}

/// Gives an int32 on the stack the width an eight-byte destination reads it at.
///
/// A C# compiler always converts first, so this only fires on hand-written IL.
void
TransformData::widen_i4_to_i8 (StackInfo *sp, MonoType *type)
{
	if (sp->type != StackType::I4 || mint_type (type) != MintType::I8)
		return;

	// The int32 wrote four bytes of the stack slot and the destination takes all
	// eight, so the value needs an extension first. ECMA-335 Table III.9 gives
	// the rule for a call argument, and the other assignments follow. A native
	// unsigned int is filled with zeroes, and everything else with the sign.
	int op =
		mini_get_underlying_type (type)->type == MONO_TYPE_U ? MINT_CONV_I8_U4 : MINT_CONV_I8_I4;
	interp_add_conv (sp, NULL, StackType::I8, op);
}

/// Gives a native int index the int32 form the indexing opcodes read.
void
TransformData::narrow_index (StackInfo *sp)
{
	if (sp->type == StackType::I8)
		// ECMA-335 III.4.8 lets an array index be an int32 or a native int, and
		// switch compares its operand as an unsigned integer. Both opcodes read
		// four bytes, so an index left wide loses its high bytes there and
		// selects an element instead of failing the bound test.
		interp_add_conv (sp, NULL, StackType::I4, MINT_CONV_INDEX_I8);
}

/*
 * Give the value at sp the precision a destination of type dtype holds it at.
 *
 * CIL has a single floating point stack type, so a value produced as r4 can
 * legally be stored where an r8 is kept, and the other way round. The
 * interpreter keeps the two apart - an r4 stack slot holds four bytes and an r8
 * slot eight - so anything moving one into the other has to convert first.
 * Every other pairing is either already right or a type error, and neither is
 * this to fix.
 */
void
TransformData::coerce_fp (StackInfo *sp, std::optional<StackType> dtype)
{
	if (dtype == StackType::R8 && sp->type == StackType::R4)
		interp_add_conv (sp, NULL, StackType::R8, MINT_CONV_R8_R4);
	else if (dtype == StackType::R4 && sp->type == StackType::R8)
		interp_add_conv (sp, NULL, StackType::R4, MINT_CONV_R4_R8);
}

/// The stack type a value of this type is held at, for the floating point
/// types only. Returns nothing for anything else, which coerce_fp leaves
/// alone.
std::optional<StackType>
fp_stack_type (MonoType *type)
{
	if (type->byref)
		return std::nullopt;

	type = mini_get_underlying_type (type);

	switch (type->type) {
	case MONO_TYPE_R4:
		return StackType::R4;
	case MONO_TYPE_R8:
		return StackType::R8;
	default:
		return std::nullopt;
	}
}

void
TransformData::two_arg_branch (int mint_op, int offset, int inst_size)
{
	StackType type1 =
		sp[-1].type == StackType::O || sp[-1].type == StackType::MP ? StackType::I : sp[-1].type;
	StackType type2 =
		sp[-2].type == StackType::O || sp[-2].type == StackType::MP ? StackType::I : sp[-2].type;
	CHECK_STACK (2);

	if (type1 == StackType::I4 && type2 == StackType::I8) {
		// The il instruction starts with the actual branch, and not with the conversion opcodes
		interp_add_conv (sp - 1, last_ins, StackType::I8, MINT_CONV_I8_I4);
		type1 = StackType::I8;
	} else if (type1 == StackType::I8 && type2 == StackType::I4) {
		interp_add_conv (sp - 2, last_ins, StackType::I8, MINT_CONV_I8_I4);
	} else if (type1 == StackType::R4 && type2 == StackType::R8) {
		interp_add_conv (sp - 1, last_ins, StackType::R8, MINT_CONV_R8_R4);
		type1 = StackType::R8;
	} else if (type1 == StackType::R8 && type2 == StackType::R4) {
		interp_add_conv (sp - 2, last_ins, StackType::R8, MINT_CONV_R8_R4);
	} else if (type1 != type2) {
		g_warning ("%s.%s: branch type mismatch %d %d", m_class_get_name (method->klass),
		           method->name, (int) sp[-1].type, (int) sp[-2].type);
	}

	int long_op = op_for_stack_type (mint_op, type1);
	int short_op = long_op + MINT_BEQ_I4_S - MINT_BEQ_I4;
	sp -= 2;
	if (offset) {
		handle_branch (short_op, long_op, offset + inst_size);
		interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
	} else {
		interp_add_ins (MINT_NOP);
	}
}

void
TransformData::unary_arith_op (int mint_op)
{
	int op = op_for_stack_type (mint_op, sp[-1].type);
	CHECK_STACK (1);
	sp--;
	interp_add_ins (op);
	interp_ins_set_sreg (last_ins, sp[0].local);
	push_simple_type (sp[0].type);
	interp_ins_set_dreg (last_ins, sp[-1].local);
}

void
TransformData::binary_arith_op (int mint_op)
{
	StackType type1 = sp[-2].type;
	StackType type2 = sp[-1].type;
	int op;
#if SIZEOF_VOID_P == 8
	if ((type1 == StackType::MP || type1 == StackType::I8) && type2 == StackType::I4) {
		interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_I4);
		type2 = StackType::I8;
	}
	if (type1 == StackType::I4 && (type2 == StackType::MP || type2 == StackType::I8)) {
		interp_add_conv (sp - 2, NULL, StackType::I8, MINT_CONV_I8_I4);
		type1 = StackType::I8;
	}
#endif
	if (type1 == StackType::R8 && type2 == StackType::R4) {
		interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R8_R4);
		type2 = StackType::R8;
	}
	if (type1 == StackType::R4 && type2 == StackType::R8) {
		interp_add_conv (sp - 2, NULL, StackType::R8, MINT_CONV_R8_R4);
		type1 = StackType::R8;
	}
	if (type1 == StackType::MP)
		type1 = StackType::I;
	if (type2 == StackType::MP)
		type2 = StackType::I;
	if (type1 != type2) {
		g_warning ("%s.%s: %04x arith type mismatch %s %d %d", m_class_get_name (method->klass),
		           method->name, ip - il_code, opname (mint_op), type1, type2);
	}
	op = op_for_stack_type (mint_op, type1);
	CHECK_STACK (2);
	sp -= 2;
	interp_add_ins (op);
	interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
	push_simple_type (type1);
	interp_ins_set_dreg (last_ins, sp[-1].local);
}

void
TransformData::shift_op (int mint_op)
{
	int op = op_for_stack_type (mint_op, sp[-2].type);
	CHECK_STACK (2);
	if (sp[-1].type != StackType::I4) {
		g_warning ("%s.%s: shift type mismatch %d", m_class_get_name (method->klass), method->name,
		           sp[-2].type);
	}
	sp -= 2;
	interp_add_ins (op);
	interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
	push_simple_type (sp[0].type);
	interp_ins_set_dreg (last_ins, sp[-1].local);
}

void
TransformData::interp_generate_mae_throw (MonoMethod *method, MonoMethod *target_method)
{
	MonoJitICallInfo *info = &mono_get_jit_icall_info ()->mono_throw_method_access;

	/* Inject code throwing MethodAccessException */
	interp_add_ins (MINT_MONO_LDPTR);
	push_simple_type (StackType::I);
	interp_ins_set_dreg (last_ins, sp[-1].local);
	last_ins->data[0] = get_data_item_index (method);
	locals[sp[-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;

	interp_add_ins (MINT_MONO_LDPTR);
	push_simple_type (StackType::I);
	interp_ins_set_dreg (last_ins, sp[-1].local);
	last_ins->data[0] = get_data_item_index (target_method);
	locals[sp[-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;

	sp -= 2;
	interp_add_ins (MINT_ICALL_PP_V);
	interp_ins_set_dreg (last_ins, sp[0].local);
	last_ins->data[0] = get_data_item_index ((gpointer) info->func);
}

void
TransformData::interp_generate_bie_throw ()
{
	MonoJitICallInfo *info = &mono_get_jit_icall_info ()->mono_throw_bad_image;

	interp_add_ins (MINT_ICALL_V_V);
	// Allocate a dummy local to serve as dreg for this instruction
	push_simple_type (StackType::I4);
	sp--;
	interp_ins_set_dreg (last_ins, sp[0].local);
	last_ins->data[0] = get_data_item_index ((gpointer) info->func);
}

void
TransformData::interp_generate_not_supported_throw ()
{
	MonoJitICallInfo *info = &mono_get_jit_icall_info ()->mono_throw_not_supported;

	interp_add_ins (MINT_ICALL_V_V);
	// Allocate a dummy local to serve as dreg for this instruction
	push_simple_type (StackType::I4);
	sp--;
	interp_ins_set_dreg (last_ins, sp[0].local);
	last_ins->data[0] = get_data_item_index ((gpointer) info->func);
}

void
TransformData::interp_generate_ipe_throw_with_msg (MonoError *error_msg)
{
	MonoJitICallInfo *info = &mono_get_jit_icall_info ()->mono_throw_invalid_program;

	char *msg = mono_mem_manager_strdup (mem_manager, mono_error_get_message (error_msg));

	interp_add_ins (MINT_MONO_LDPTR);
	push_simple_type (StackType::I);
	interp_ins_set_dreg (last_ins, sp[-1].local);
	locals[sp[-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
	last_ins->data[0] = get_data_item_index (msg);

	sp -= 1;
	interp_add_ins (MINT_ICALL_P_V);
	interp_ins_set_dreg (last_ins, sp[0].local);
	last_ins->data[0] = get_data_item_index ((gpointer) info->func);
}

/// Returns method's header, or null if it has none.
///
/// mono_method_get_header_internal () sets an error for a method with no
/// body - an abstract method or an icall - and this returns null instead,
/// without setting one.
MonoMethodHeader *
interp_method_get_header (MonoMethod *method, MonoError *error)
{
	if (mono_method_has_no_body (method))
		return NULL;
	else
		return mono_method_get_header_internal (method, error);
}

gboolean
TransformData::interp_ip_in_cbb (int il_offset)
{
	InterpBasicBlock *bb = offset_to_bb[il_offset];

	return bb == NULL || bb == cbb;
}

gboolean
interp_ins_is_ldc (InterpInst *ins)
{
	return ins->opcode >= MINT_LDC_I4_M1 && ins->opcode <= MINT_LDC_I8;
}

gint32
interp_get_const_from_ldc_i4 (InterpInst *ins)
{
	switch (ins->opcode) {
	case MINT_LDC_I4_M1:
		return -1;
	case MINT_LDC_I4_0:
		return 0;
	case MINT_LDC_I4_1:
		return 1;
	case MINT_LDC_I4_2:
		return 2;
	case MINT_LDC_I4_3:
		return 3;
	case MINT_LDC_I4_4:
		return 4;
	case MINT_LDC_I4_5:
		return 5;
	case MINT_LDC_I4_6:
		return 6;
	case MINT_LDC_I4_7:
		return 7;
	case MINT_LDC_I4_8:
		return 8;
	case MINT_LDC_I4_S:
		return (gint32) (gint8) ins->data[0];
	case MINT_LDC_I4:
		return READ32 (&ins->data[0]);
	default:
		g_assert_not_reached ();
	}
}

/* If ins is not null, it will replace it with the ldc */
InterpInst *
TransformData::interp_get_ldc_i4_from_const (InterpInst *ins, gint32 ct, int dreg)
{
	int opcode;
	switch (ct) {
	case -1:
		opcode = MINT_LDC_I4_M1;
		break;
	case 0:
		opcode = MINT_LDC_I4_0;
		break;
	case 1:
		opcode = MINT_LDC_I4_1;
		break;
	case 2:
		opcode = MINT_LDC_I4_2;
		break;
	case 3:
		opcode = MINT_LDC_I4_3;
		break;
	case 4:
		opcode = MINT_LDC_I4_4;
		break;
	case 5:
		opcode = MINT_LDC_I4_5;
		break;
	case 6:
		opcode = MINT_LDC_I4_6;
		break;
	case 7:
		opcode = MINT_LDC_I4_7;
		break;
	case 8:
		opcode = MINT_LDC_I4_8;
		break;
	default:
		if (ct >= -128 && ct <= 127)
			opcode = MINT_LDC_I4_S;
		else
			opcode = MINT_LDC_I4;
		break;
	}

	int new_size = oplen (opcode);

	if (ins == NULL)
		ins = interp_add_ins (opcode);

	int ins_size = oplen (ins->opcode);
	if (ins_size < new_size) {
		// We can't replace the passed instruction, discard it and emit a new one
		ins = interp_insert_ins (ins, opcode);
		interp_clear_ins (ins->prev);
	} else {
		ins->opcode = opcode;
	}
	interp_ins_set_dreg (ins, dreg);

	if (new_size == 3)
		ins->data[0] = (gint8) ct;
	else if (new_size == 4)
		WRITE32_INS (ins, 0, &ct);

	return ins;
}

InterpInst *
TransformData::interp_inst_replace_with_i8_const (InterpInst *ins, gint64 ct)
{
	int size = oplen (ins->opcode);
	int dreg = ins->dreg;

	if (size < 5) {
		ins = interp_insert_ins (ins, MINT_LDC_I8);
		interp_clear_ins (ins->prev);
	} else {
		ins->opcode = MINT_LDC_I8;
	}
	WRITE64_INS (ins, 0, &ct);
	ins->dreg = dreg;

	return ins;
}

int
interp_get_ldind_for_mt (MintType mt)
{
	switch (mt) {
	case MintType::I1:
		return MINT_LDIND_I1_CHECK;
	case MintType::U1:
		return MINT_LDIND_U1_CHECK;
	case MintType::I2:
		return MINT_LDIND_I2_CHECK;
	case MintType::U2:
		return MINT_LDIND_U2_CHECK;
	case MintType::I4:
		return MINT_LDIND_I4_CHECK;
	case MintType::I8:
		return MINT_LDIND_I8_CHECK;
	case MintType::R4:
		return MINT_LDIND_R4_CHECK;
	case MintType::R8:
		return MINT_LDIND_R8_CHECK;
	case MintType::O:
		return MINT_LDIND_REF;
	default:
		g_assert_not_reached ();
	}
	return -1;
}

void
TransformData::interp_emit_ldobj (MonoClass *klass)
{
	MintType mt = mint_type (m_class_get_byval_arg (klass));
	gint32 size;
	sp--;

	if (mt == MintType::VT) {
		interp_add_ins (MINT_LDOBJ_VT);
		size = mono_class_value_size (klass, NULL);
		g_assert (size < G_MAXUINT16);
		interp_ins_set_sreg (last_ins, sp[0].local);
		push_type_vt (klass, size);
	} else {
		int opcode = interp_get_ldind_for_mt (mt);
		interp_add_ins (opcode);
		interp_ins_set_sreg (last_ins, sp[0].local);
		push_type (stack_type_of (mt), klass);
	}
	interp_ins_set_dreg (last_ins, sp[-1].local);
	if (mt == MintType::VT)
		last_ins->data[0] = size;
}

void
TransformData::interp_emit_stobj (MonoClass *klass)
{
	MintType mt = mint_type (m_class_get_byval_arg (klass));

	coerce_fp (sp - 1, stack_type_of (mt));

	if (mt == MintType::VT) {
		interp_add_ins (MINT_STOBJ_VT);
		last_ins->data[0] = get_data_item_index (klass);
	} else {
		int opcode;
		switch (mt) {
		case MintType::I1:
		case MintType::U1:
			opcode = MINT_STIND_I1;
			break;
		case MintType::I2:
		case MintType::U2:
			opcode = MINT_STIND_I2;
			break;
		case MintType::I4:
			opcode = MINT_STIND_I4;
			break;
		case MintType::I8:
			opcode = MINT_STIND_I8;
			break;
		case MintType::R4:
			opcode = MINT_STIND_R4;
			break;
		case MintType::R8:
			opcode = MINT_STIND_R8;
			break;
		case MintType::O:
			opcode = MINT_STIND_REF;
			break;
		default:
			g_assert_not_reached ();
			break;
		}
		interp_add_ins (opcode);
	}
	sp -= 2;
	interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
}

void
TransformData::interp_emit_ldelema (MonoClass *array_class, MonoClass *check_class)
{
	MonoClass *element_class = m_class_get_element_class (array_class);
	int rank = m_class_get_rank (array_class);
	int size = mono_class_array_element_size (element_class);
	gboolean call_args = FALSE;

	gboolean bounded = m_class_get_byval_arg (array_class)
	                       ? m_class_get_byval_arg (array_class)->type == MONO_TYPE_ARRAY
	                       : FALSE;

	sp -= rank + 1;
	// We only need type checks when writing to array of references
	if (!check_class || m_class_is_valuetype (element_class)) {
		if (rank == 1 && !bounded) {
			interp_add_ins (MINT_LDELEMA1);
			interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
			g_assert (size < G_MAXUINT16);
			last_ins->data[0] = size;
		} else {
			interp_add_ins (MINT_LDELEMA);
			for (int i = 0; i < rank + 1; i++)
				locals[sp[i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
			last_ins->data[0] = rank;
			g_assert (size < G_MAXUINT16);
			last_ins->data[1] = size;
			call_args = TRUE;
		}
	} else {
		interp_add_ins (MINT_LDELEMA_TC);
		for (int i = 0; i < rank + 1; i++)
			locals[sp[i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
		last_ins->data[0] = get_data_item_index (check_class);
		call_args = TRUE;
	}

	push_simple_type (StackType::MP);
	interp_ins_set_dreg (last_ins, sp[-1].local);
	if (call_args)
		locals[sp[-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
}

void
TransformData::interp_emit_memory_barrier (int kind)
{
#if defined(TARGET_WASM)
	// mono_memory_barrier is dummy on wasm
#elif defined(TARGET_X86) || defined(TARGET_AMD64)
	if (kind == MONO_MEMORY_BARRIER_SEQ)
		interp_add_ins (MINT_MONO_MEMORY_BARRIER);
#else
	interp_add_ins (MINT_MONO_MEMORY_BARRIER);
#endif
}

gboolean
type_has_references (MonoType *type)
{
	if (MONO_TYPE_IS_REFERENCE (type))
		return TRUE;
	if (MONO_TYPE_ISSTRUCT (type)) {
		MonoClass *klass = mono_class_from_mono_type_internal (type);
		if (!m_class_is_inited (klass))
			mono_class_init_internal (klass);
		return m_class_has_references (klass);
	}
	return FALSE;
}

#ifdef NO_UNALIGNED_ACCESS
static int
get_unaligned_opcode (int opcode)
{
	switch (opcode) {
	case MINT_LDFLD_I8:
		return MINT_LDFLD_I8_UNALIGNED;
	case MINT_LDFLD_R8:
		return MINT_LDFLD_R8_UNALIGNED;
	case MINT_STFLD_I8:
		return MINT_STFLD_I8_UNALIGNED;
	case MINT_STFLD_R8:
		return MINT_STFLD_R8_UNALIGNED;
	default:
		g_assert_not_reached ();
	}
	return -1;
}
#endif

static MonoJumpInfoType
patch_kind_for (MonoRgctxInfoType info_type)
{
	switch (info_type) {
	case MONO_RGCTX_INFO_STATIC_DATA:
	case MONO_RGCTX_INFO_KLASS:
	case MONO_RGCTX_INFO_VTABLE:
	case MONO_RGCTX_INFO_CAST_CACHE:
	case MONO_RGCTX_INFO_REFLECTION_TYPE:
		return MONO_PATCH_INFO_CLASS;
	case MONO_RGCTX_INFO_METHOD:
	case MONO_RGCTX_INFO_GENERIC_METHOD_CODE:
		return MONO_PATCH_INFO_METHODCONST;
	case MONO_RGCTX_INFO_CLASS_FIELD:
		return MONO_PATCH_INFO_FIELD;
	default:
		g_assert_not_reached ();
	}
}

/*
 * This restates MethodLLVMEmitter::rgctx_fetch () for the interpreter, so a slot
 * one tier allocated is the slot the other reads. It is shorter because
 * shared_form () only shares a method that reads its context out of a receiver,
 * so every entry goes in the class rgctx an instantiation's vtable carries.
 */
int
TransformData::emit_rgctx_fetch (MonoRgctxInfoType info_type, gpointer data)
{
	// An entry the class rgctx cannot answer needs an MRGCTX, which arrives as
	// an argument no interpreter entry carries.
	if (rgctx_receiver_local < 0 || mini_method_needs_mrgctx (method)) {
		cannot_share ("a generic context with no receiver to read it from");
		return -1;
	}

	MonoJumpInfo patch {};
	MonoJumpInfoRgctxEntry lookup {};

	patch.type = patch_kind_for (info_type);
	patch.data.target = data;

	lookup.d.klass = method->klass;
	lookup.in_mrgctx = FALSE;
	lookup.data = &patch;
	lookup.info_type = info_type;

	int slot = mini_get_rgctx_entry_slot (&lookup);

	// in_mrgctx was FALSE, so the slot is the class rgctx's and the index below
	// is what mono_class_fill_runtime_generic_context () takes.
	g_assert (!MONO_RGCTX_SLOT_IS_MRGCTX (slot));

	int index = MONO_RGCTX_SLOT_INDEX (slot);
	int dreg = create_interp_local (mono_get_int_type ());

	interp_add_ins (MINT_RGCTX_FETCH);
	interp_ins_set_dreg (last_ins, dreg);
	interp_ins_set_sreg (last_ins, rgctx_receiver_local);
	WRITE32_INS (last_ins, 0, &index);

	return dreg;
}

void
TransformData::interp_handle_isinst_dyn (int klass_local, MonoClass *klass, gboolean isinst_instr)
{
	interp_add_ins (isinst_instr ? MINT_ISINST_DYN : MINT_CASTCLASS_DYN);
	sp--;
	interp_ins_set_sregs2 (last_ins, sp[0].local, klass_local);
	if (isinst_instr)
		push_type (sp[0].type, sp[0].klass);
	else
		push_type (StackType::O, klass);
	interp_ins_set_dreg (last_ins, sp[-1].local);

	ip += 5;
}

void
TransformData::interp_handle_isinst (MonoClass *klass, gboolean isinst_instr)
{
	if (!mono_class_has_variant_generic_params (klass)) {
		if (mono_class_is_interface (klass))
			interp_add_ins (isinst_instr ? MINT_ISINST_INTERFACE : MINT_CASTCLASS_INTERFACE);
		else if (!mono_class_is_marshalbyref (klass) && m_class_get_rank (klass) == 0
		         && !mono_class_is_nullable (klass))
			interp_add_ins (isinst_instr ? MINT_ISINST_COMMON : MINT_CASTCLASS_COMMON);
		else
			interp_add_ins (isinst_instr ? MINT_ISINST : MINT_CASTCLASS);
	} else {
		interp_add_ins (isinst_instr ? MINT_ISINST : MINT_CASTCLASS);
	}
	sp--;
	interp_ins_set_sreg (last_ins, sp[0].local);
	if (isinst_instr)
		push_type (sp[0].type, sp[0].klass);
	else
		push_type (StackType::O, klass);
	interp_ins_set_dreg (last_ins, sp[-1].local);
	last_ins->data[0] = get_data_item_index (klass);

	ip += 5;
}

void
TransformData::interp_emit_ldsflda (MonoClassField *field, MonoError *error)
{
	MonoDomain *domain = rtm->domain;
	// Initialize the offset for the field
	MonoVTable *vtable = mono_class_vtable_checked (domain, field->parent, error);
	return_if_nok (error);

	push_simple_type (StackType::MP);
	if (mono_class_field_is_special_static (field)) {
		guint32 offset;

		mono_domain_lock (domain);
		g_assert (domain->special_static_fields);
		offset = GPOINTER_TO_UINT (g_hash_table_lookup (domain->special_static_fields, field));
		mono_domain_unlock (domain);
		g_assert (offset);

		interp_add_ins (MINT_LDSSFLDA);
		interp_ins_set_dreg (last_ins, sp[-1].local);
		WRITE32_INS (last_ins, 0, &offset);
		last_ins->data[2] = get_data_item_index (vtable);
	} else {
		interp_add_ins (MINT_LDSFLDA);
		interp_ins_set_dreg (last_ins, sp[-1].local);
		last_ins->data[0] = get_data_item_index (vtable);
		last_ins->data[1] = get_data_item_index ((char *) mono_vtable_get_static_field_data (vtable)
		                                         + field->offset);
	}
}

gboolean
TransformData::interp_emit_load_const (gpointer field_addr, MintType mt)
{
	if (mt == MintType::VT)
		return FALSE;

	push_simple_type (stack_type_of (mt));
	if ((mt >= MintType::I1 && mt <= MintType::I4)) {
		gint32 val;
		switch (mt) {
		case MintType::I1:
			val = *(gint8 *) field_addr;
			break;
		case MintType::U1:
			val = *(guint8 *) field_addr;
			break;
		case MintType::I2:
			val = *(gint16 *) field_addr;
			break;
		case MintType::U2:
			val = *(guint16 *) field_addr;
			break;
		default:
			val = *(gint32 *) field_addr;
		}
		interp_get_ldc_i4_from_const (NULL, val, sp[-1].local);
	} else if (mt == MintType::I8) {
		gint64 val = *(gint64 *) field_addr;
		interp_add_ins (MINT_LDC_I8);
		interp_ins_set_dreg (last_ins, sp[-1].local);
		WRITE64_INS (last_ins, 0, &val);
	} else if (mt == MintType::R4) {
		float val = *(float *) field_addr;
		interp_add_ins (MINT_LDC_R4);
		interp_ins_set_dreg (last_ins, sp[-1].local);
		WRITE32_INS (last_ins, 0, &val);
	} else if (mt == MintType::R8) {
		double val = *(double *) field_addr;
		interp_add_ins (MINT_LDC_R8);
		interp_ins_set_dreg (last_ins, sp[-1].local);
		WRITE64_INS (last_ins, 0, &val);
	} else {
		// Revert stack
		sp--;
		return FALSE;
	}
	return TRUE;
}

/*
 * Converts the value on top of the stack to what a destination of type ftype
 * holds it at, where the two differ only in width.
 */
void
TransformData::emit_convert (MonoType *ftype)
{
	ftype = mini_get_underlying_type (ftype);

	switch (ftype->type) {
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
		coerce_fp (sp - 1, fp_stack_type (ftype));
		break;
	default:
		widen_i4_to_i8 (sp - 1, ftype);
		break;
	}
}

void
TransformData::interp_emit_sfld_access (MonoClassField *field, MonoClass *field_class, MintType mt,
                                        gboolean is_load, MonoError *error)
{
	MonoDomain *domain = rtm->domain;
	// Initialize the offset for the field
	MonoVTable *vtable = mono_class_vtable_checked (domain, field->parent, error);
	return_if_nok (error);

	if (mono_class_field_is_special_static (field)) {
		guint32 offset;

		mono_domain_lock (domain);
		g_assert (domain->special_static_fields);
		offset = GPOINTER_TO_UINT (g_hash_table_lookup (domain->special_static_fields, field));
		mono_domain_unlock (domain);
		g_assert (offset);

		/*
		 * The vtable rides along in the last operand because the offset alone
		 * does not say which class owns the storage, and the vtable is the only
		 * source the handler has for running the class initializer. The first
		 * access to a special static field from outside its own class is
		 * exactly when that initializer has to run.
		 */
		int vtable_index = get_data_item_index (vtable);

		// Offset is SpecialStaticOffset
		if ((offset & 0x80000000) == 0 && mt != MintType::VT) {
			// This field is thread static
			if (is_load) {
				interp_add_ins (op_for_mint_type (MINT_LDTSFLD_I1, mt));
				WRITE32_INS (last_ins, 0, &offset);
				push_type (stack_type_of (mt), field_class);
				interp_ins_set_dreg (last_ins, sp[-1].local);
			} else {
				interp_add_ins (op_for_mint_type (MINT_STTSFLD_I1, mt));
				WRITE32_INS (last_ins, 0, &offset);
				sp--;
				interp_ins_set_sreg (last_ins, sp[0].local);
			}
			last_ins->data[2] = vtable_index;
		} else {
			if (mt == MintType::VT) {
				int size = mono_class_value_size (field_class, NULL);
				g_assert (size < G_MAXUINT16);
				if (is_load) {
					interp_add_ins (MINT_LDSSFLD_VT);
					push_type_vt (field_class, size);
					interp_ins_set_dreg (last_ins, sp[-1].local);
				} else {
					interp_add_ins (MINT_STSSFLD_VT);
					sp--;
					interp_ins_set_sreg (last_ins, sp[0].local);
				}
				WRITE32_INS (last_ins, 0, &offset);
				last_ins->data[2] = size;
				last_ins->data[3] = vtable_index;
			} else {
				if (is_load) {
					interp_add_ins (MINT_LDSSFLD);
					push_type (stack_type_of (mt), field_class);
					interp_ins_set_dreg (last_ins, sp[-1].local);
				} else {
					interp_add_ins (MINT_STSSFLD);
					sp--;
					interp_ins_set_sreg (last_ins, sp[0].local);
				}
				last_ins->data[0] = get_data_item_index (field);
				WRITE32_INS (last_ins, 1, &offset);
				last_ins->data[3] = vtable_index;
			}
		}
	} else {
		gpointer field_addr = (char *) mono_vtable_get_static_field_data (vtable) + field->offset;
		int size = 0;
		if (mt == MintType::VT)
			size = mono_class_value_size (field_class, NULL);
		if (is_load) {
			MonoType *ftype = mono_field_get_type_internal (field);
			if (ftype->attrs & FIELD_ATTRIBUTE_INIT_ONLY && vtable->initialized) {
				if (interp_emit_load_const (field_addr, mt))
					return;
			}
			if (mt == MintType::VT) {
				interp_add_ins (MINT_LDSFLD_VT);
				push_type_vt (field_class, size);
			} else {
				interp_add_ins (op_for_mint_type (MINT_LDSFLD_I1, mt));
				push_type (stack_type_of (mt), field_class);
			}
			interp_ins_set_dreg (last_ins, sp[-1].local);
		} else {
			interp_add_ins ((mt == MintType::VT) ? MINT_STSFLD_VT
			                                     : op_for_mint_type (MINT_STSFLD_I1, mt));
			sp--;
			interp_ins_set_sreg (last_ins, sp[0].local);
		}

		last_ins->data[0] = get_data_item_index (vtable);
		last_ins->data[1] = get_data_item_index ((char *) field_addr);
		if (mt == MintType::VT)
			last_ins->data[2] = size;
	}
}

void
TransformData::initialize_clause_bblocks ()
{
	MonoMethodHeader *header = this->header;
	int i;

	for (i = 0; i < header->code_size; i++)
		clause_indexes[i] = -1;

	for (i = 0; i < header->num_clauses; i++) {
		MonoExceptionClause *c = header->clauses + i;
		InterpBasicBlock *bb;

		for (int j = c->handler_offset; j < c->handler_offset + c->handler_len; j++) {
			if (clause_indexes[j] == -1)
				clause_indexes[j] = i;
		}

		bb = offset_to_bb[c->try_offset];
		g_assert (bb);
		bb->eh_block = TRUE;

		/* We never inline methods with clauses, so we can hard code stack heights */
		bb = offset_to_bb[c->handler_offset];
		g_assert (bb);
		bb->eh_block = TRUE;

		if (c->flags == MONO_EXCEPTION_CLAUSE_FINALLY) {
			bb->stack_height = 0;
		} else {
			bb->stack_height = 1;
			bb->stack_state = arena.create<StackInfo> ();
			bb->stack_state[0].type = StackType::O;
			bb->stack_state[0].klass = NULL; /*FIX*/
			bb->stack_state[0].size = MINT_STACK_SLOT_SIZE;
			bb->stack_state[0].offset = 0;
			bb->stack_state[0].local =
				create_interp_stack_local (StackType::O, NULL, MINT_STACK_SLOT_SIZE, 0);
		}

		if (c->flags == MONO_EXCEPTION_CLAUSE_FILTER) {
			bb = offset_to_bb[c->data.filter_offset];
			g_assert (bb);
			bb->eh_block = TRUE;
			bb->stack_height = 1;
			bb->stack_state = arena.create<StackInfo> ();
			bb->stack_state[0].type = StackType::O;
			bb->stack_state[0].klass = NULL; /*FIX*/
			bb->stack_state[0].size = MINT_STACK_SLOT_SIZE;
			bb->stack_state[0].offset = 0;
			bb->stack_state[0].local =
				create_interp_stack_local (StackType::O, NULL, MINT_STACK_SLOT_SIZE, 0);
		} else if (c->flags == MONO_EXCEPTION_CLAUSE_NONE) {
			/*
			 * JIT doesn't emit sdb seq intr point at the start of catch clause, probably
			 * by accident. Mimic the same behavior with the interpreter for now. Because
			 * this bb is not empty, we won't emit a MINT_SDB_INTR_LOC when generating the code
			 */
			interp_insert_ins_bb (bb, NULL, MINT_NOP);
		}
	}
}

void
TransformData::handle_ldind (int op, StackType type, gboolean *volatile_)
{
	CHECK_STACK (1);
	interp_add_ins (op);
	sp--;
	interp_ins_set_sreg (last_ins, sp[0].local);
	push_simple_type (type);
	interp_ins_set_dreg (last_ins, sp[-1].local);

	if (*volatile_) {
		interp_emit_memory_barrier (MONO_MEMORY_BARRIER_ACQ);
		*volatile_ = FALSE;
	}
	++ip;
}

void
TransformData::handle_stind (int op, gboolean *volatile_)
{
	CHECK_STACK (2);
	if (op == MINT_STIND_R4 || op == MINT_STIND_R8)
		coerce_fp (sp - 1, op == MINT_STIND_R4 ? StackType::R4 : StackType::R8);
	if (op == MINT_STIND_I)
		widen_i4_to_i8 (sp - 1, mono_get_int_type ());
	if (op == MINT_STIND_I8)
		widen_i4_to_i8 (sp - 1, m_class_get_byval_arg (mono_defaults.int64_class));
	if (*volatile_) {
		interp_emit_memory_barrier (MONO_MEMORY_BARRIER_REL);
		*volatile_ = FALSE;
	}
	interp_add_ins (op);
	sp -= 2;
	interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);

	++ip;
}

void
TransformData::handle_ldelem (int op, StackType type)
{
	CHECK_STACK (2);
	narrow_index (sp - 1);
	interp_add_ins (op);
	sp -= 2;
	interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
	push_simple_type (type);
	interp_ins_set_dreg (last_ins, sp[-1].local);
	++ip;
}

void
TransformData::handle_stelem (int op)
{
	CHECK_STACK (3);
	if (op == MINT_STELEM_R4 || op == MINT_STELEM_R8)
		coerce_fp (sp - 1, op == MINT_STELEM_R4 ? StackType::R4 : StackType::R8);
	if (op == MINT_STELEM_I)
		widen_i4_to_i8 (sp - 1, mono_get_int_type ());
	if (op == MINT_STELEM_I8)
		widen_i4_to_i8 (sp - 1, m_class_get_byval_arg (mono_defaults.int64_class));
	narrow_index (sp - 2);
	interp_add_ins (op);
	sp -= 3;
	interp_ins_set_sregs3 (last_ins, sp[0].local, sp[1].local, sp[2].local);
	++ip;
}

} // namespace mono::interp
