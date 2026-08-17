/**
 * \file
 * transform CIL into different opcodes for more
 * efficient interpretation
 *
 * Written by Bernie Solomon (bernard@ugsolutions.com)
 * Copyright (c) 2004.
 */

#include "config.h"
#include <string.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/exception-internals.h>
#include <mono/metadata/metadata-update.h>
#include <mono/metadata/mono-endian.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/profiler-private.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/mono-basic-block.h>
#include <mono/metadata/abi-details.h>
#include <mono/metadata/reflection-internals.h>
#include <mono/utils/unlocked.h>
#include <mono/utils/mono-memory-model.h>

#include <mono/mini/mini.h>
#include <mono/mini/mini-runtime.h>

#include "mintops.hpp"
#include "interp-internals.hpp"
#include "interp.h"
#include "transform.hpp"
#include "transform-internal.hpp"

#include <algorithm>
#include <optional>

#include "mono/llvm/runtime.h"

/* Outside the namespace, because interp-internals.hpp declares it there. */
MonoInterpStats mono_interp_stats;

namespace mono::interp {

#define DEBUG 0

#if SIZEOF_VOID_P == 8
#define MINT_NEG_P MINT_NEG_I8
#define MINT_NOT_P MINT_NOT_I8

#define MINT_NEG_FP MINT_NEG_R8

#define MINT_ADD_P MINT_ADD_I8
#define MINT_SUB_P MINT_SUB_I8
#define MINT_MUL_P MINT_MUL_I8
#define MINT_DIV_P MINT_DIV_I8
#define MINT_DIV_UN_P MINT_DIV_UN_I8
#define MINT_REM_P MINT_REM_I8
#define MINT_REM_UN_P MINT_REM_UN_I8
#define MINT_AND_P MINT_AND_I8
#define MINT_OR_P MINT_OR_I8
#define MINT_XOR_P MINT_XOR_I8
#define MINT_SHL_P MINT_SHL_I8
#define MINT_SHR_P MINT_SHR_I8
#define MINT_SHR_UN_P MINT_SHR_UN_I8

#define MINT_CEQ_P MINT_CEQ_I8
#define MINT_CNE_P MINT_CNE_I8
#define MINT_CLT_P MINT_CLT_I8
#define MINT_CLT_UN_P MINT_CLT_UN_I8
#define MINT_CGT_P MINT_CGT_I8
#define MINT_CGT_UN_P MINT_CGT_UN_I8
#define MINT_CLE_P MINT_CLE_I8
#define MINT_CLE_UN_P MINT_CLE_UN_I8
#define MINT_CGE_P MINT_CGE_I8
#define MINT_CGE_UN_P MINT_CGE_UN_I8

#define MINT_ADD_FP MINT_ADD_R8
#define MINT_SUB_FP MINT_SUB_R8
#define MINT_MUL_FP MINT_MUL_R8
#define MINT_DIV_FP MINT_DIV_R8
#define MINT_REM_FP MINT_REM_R8

#define MINT_CNE_FP MINT_CNE_R8
#define MINT_CEQ_FP MINT_CEQ_R8
#define MINT_CGT_FP MINT_CGT_R8
#define MINT_CGE_FP MINT_CGE_R8
#define MINT_CLT_FP MINT_CLT_R8
#define MINT_CLE_FP MINT_CLE_R8

#define MINT_CONV_OVF_U4_P MINT_CONV_OVF_U4_I8
#else

#define MINT_NEG_P MINT_NEG_I4
#define MINT_NOT_P MINT_NOT_I4

#define MINT_NEG_FP MINT_NEG_R4

#define MINT_ADD_P MINT_ADD_I4
#define MINT_SUB_P MINT_SUB_I4
#define MINT_MUL_P MINT_MUL_I4
#define MINT_DIV_P MINT_DIV_I4
#define MINT_DIV_UN_P MINT_DIV_UN_I4
#define MINT_REM_P MINT_REM_I4
#define MINT_REM_UN_P MINT_REM_UN_I4
#define MINT_AND_P MINT_AND_I4
#define MINT_OR_P MINT_OR_I4
#define MINT_XOR_P MINT_XOR_I4
#define MINT_SHL_P MINT_SHL_I4
#define MINT_SHR_P MINT_SHR_I4
#define MINT_SHR_UN_P MINT_SHR_UN_I4

#define MINT_CEQ_P MINT_CEQ_I4
#define MINT_CNE_P MINT_CNE_I4
#define MINT_CLT_P MINT_CLT_I4
#define MINT_CLT_UN_P MINT_CLT_UN_I4
#define MINT_CGT_P MINT_CGT_I4
#define MINT_CGT_UN_P MINT_CGT_UN_I4
#define MINT_CLE_P MINT_CLE_I4
#define MINT_CLE_UN_P MINT_CLE_UN_I4
#define MINT_CGE_P MINT_CGE_I4
#define MINT_CGE_UN_P MINT_CGE_UN_I4

#define MINT_ADD_FP MINT_ADD_R4
#define MINT_SUB_FP MINT_SUB_R4
#define MINT_MUL_FP MINT_MUL_R4
#define MINT_DIV_FP MINT_DIV_R4
#define MINT_REM_FP MINT_REM_R4

#define MINT_CNE_FP MINT_CNE_R4
#define MINT_CEQ_FP MINT_CEQ_R4
#define MINT_CGT_FP MINT_CGT_R4
#define MINT_CGE_FP MINT_CGE_R4
#define MINT_CLT_FP MINT_CLT_R4
#define MINT_CLE_FP MINT_CLE_R4

#define MINT_CONV_OVF_U4_P MINT_CONV_OVF_U4_I4
#endif

#if SIZEOF_VOID_P == 8
#define MINT_MOV_P MINT_MOV_8
#else
#define MINT_MOV_P MINT_MOV_4
#endif

struct MagicIntrinsic {
	const gchar *op_name;
	guint16 insn [3];
};

// static const MagicIntrinsic int_binop[] = {

static const MagicIntrinsic int_unnop[] = {
	{ "op_UnaryPlus", {MINT_MOV_P, MINT_MOV_P, MINT_MOV_4}},
	{ "op_UnaryNegation", {MINT_NEG_P, MINT_NEG_P, MINT_NEG_FP}},
	{ "op_OnesComplement", {MINT_NOT_P, MINT_NOT_P, MINT_NIY}}
};

static const MagicIntrinsic int_binop[] = {
	{ "op_Addition", {MINT_ADD_P, MINT_ADD_P, MINT_ADD_FP}},
	{ "op_Subtraction", {MINT_SUB_P, MINT_SUB_P, MINT_SUB_FP}},
	{ "op_Multiply", {MINT_MUL_P, MINT_MUL_P, MINT_MUL_FP}},
	{ "op_Division", {MINT_DIV_P, MINT_DIV_UN_P, MINT_DIV_FP}},
	{ "op_Modulus", {MINT_REM_P, MINT_REM_UN_P, MINT_REM_FP}},
	{ "op_BitwiseAnd", {MINT_AND_P, MINT_AND_P, MINT_NIY}},
	{ "op_BitwiseOr", {MINT_OR_P, MINT_OR_P, MINT_NIY}},
	{ "op_ExclusiveOr", {MINT_XOR_P, MINT_XOR_P, MINT_NIY}},
	{ "op_LeftShift", {MINT_SHL_P, MINT_SHL_P, MINT_NIY}},
	{ "op_RightShift", {MINT_SHR_P, MINT_SHR_UN_P, MINT_NIY}},
};

static const MagicIntrinsic int_cmpop[] = {
	{ "op_Inequality", {MINT_CNE_P, MINT_CNE_P, MINT_CNE_FP}},
	{ "op_Equality", {MINT_CEQ_P, MINT_CEQ_P, MINT_CEQ_FP}},
	{ "op_GreaterThan", {MINT_CGT_P, MINT_CGT_UN_P, MINT_CGT_FP}},
	{ "op_GreaterThanOrEqual", {MINT_CGE_P, MINT_CGE_UN_P, MINT_CGE_FP}},
	{ "op_LessThan", {MINT_CLT_P, MINT_CLT_UN_P, MINT_CLT_FP}},
	{ "op_LessThanOrEqual", {MINT_CLE_P, MINT_CLE_UN_P, MINT_CLE_FP}}
};

static const char *stack_type_string [] = { "I4", "I8", "R4", "R8", "O ", "VT", "MP", "F " };

/*
 * op_for_stack_type () indexes an opcode family by the numeric value of a
 * StackType, so every family it is used on has to be laid out in mintops.def in
 * the order StackType names: I4, I8, R4, R8.
 */
#define ASSERT_STACK_TYPE_FAMILY(base, suffix)                                                 \
	static_assert (base##_I8##suffix == op_for_stack_type (base##_I4##suffix, StackType::I8)    \
	                   && base##_R4##suffix == op_for_stack_type (base##_I4##suffix, StackType::R4) \
	                   && base##_R8##suffix == op_for_stack_type (base##_I4##suffix, StackType::R8), \
	               #base #suffix " is not laid out in StackType order")

ASSERT_STACK_TYPE_FAMILY (MINT_BEQ, );
ASSERT_STACK_TYPE_FAMILY (MINT_BEQ, _S);
ASSERT_STACK_TYPE_FAMILY (MINT_ADD, );
ASSERT_STACK_TYPE_FAMILY (MINT_SUB, );
ASSERT_STACK_TYPE_FAMILY (MINT_MUL, );
ASSERT_STACK_TYPE_FAMILY (MINT_DIV, );
ASSERT_STACK_TYPE_FAMILY (MINT_REM, );
ASSERT_STACK_TYPE_FAMILY (MINT_CEQ, );
ASSERT_STACK_TYPE_FAMILY (MINT_CGT, );
ASSERT_STACK_TYPE_FAMILY (MINT_CLT, );

/*
 * op_for_mint_type () does the same for the families laid out in MintType
 * order. Some of them stop at O rather than carrying a VT member, so the VT
 * check is its own macro.
 */
#define ASSERT_MINT_TYPE_FAMILY(base)                                             \
	static_assert (base##_U1 == op_for_mint_type (base##_I1, MintType::U1)        \
	                   && base##_I2 == op_for_mint_type (base##_I1, MintType::I2) \
	                   && base##_U2 == op_for_mint_type (base##_I1, MintType::U2) \
	                   && base##_I4 == op_for_mint_type (base##_I1, MintType::I4) \
	                   && base##_I8 == op_for_mint_type (base##_I1, MintType::I8) \
	                   && base##_R4 == op_for_mint_type (base##_I1, MintType::R4) \
	                   && base##_R8 == op_for_mint_type (base##_I1, MintType::R8) \
	                   && base##_O == op_for_mint_type (base##_I1, MintType::O),  \
	               #base " is not laid out in MintType order")

#define ASSERT_MINT_TYPE_FAMILY_VT(base)                                    \
	ASSERT_MINT_TYPE_FAMILY (base);                                         \
	static_assert (base##_VT == op_for_mint_type (base##_I1, MintType::VT), \
	               #base "_VT is not where MintType puts it")

ASSERT_MINT_TYPE_FAMILY_VT (MINT_LDFLD);
ASSERT_MINT_TYPE_FAMILY_VT (MINT_LDFLD_VT);
ASSERT_MINT_TYPE_FAMILY_VT (MINT_STFLD);
ASSERT_MINT_TYPE_FAMILY_VT (MINT_LDSFLD);
ASSERT_MINT_TYPE_FAMILY_VT (MINT_STSFLD);
ASSERT_MINT_TYPE_FAMILY (MINT_LDTSFLD);
ASSERT_MINT_TYPE_FAMILY (MINT_STTSFLD);

/*
 * MINT_MOV leaves the pattern after U2: everything four bytes wide and up
 * shares MINT_MOV_4 and MINT_MOV_8 rather than getting a member each.
 */
static_assert (MINT_MOV_U1 == op_for_mint_type (MINT_MOV_I1, MintType::U1)
                   && MINT_MOV_I2 == op_for_mint_type (MINT_MOV_I1, MintType::I2)
                   && MINT_MOV_U2 == op_for_mint_type (MINT_MOV_I1, MintType::U2),
               "the sign-extending MINT_MOVs are not laid out in MintType order");

/// What the eval stack holds a value of this storage type as.
static constexpr StackType
stack_type_of (MintType mt)
{
	constexpr StackType types[] = {
		StackType::I4, /*I1*/
		StackType::I4, /*U1*/
		StackType::I4, /*I2*/
		StackType::I4, /*U2*/
		StackType::I4, /*I4*/
		StackType::I8, /*I8*/
		StackType::R4, /*R4*/
		StackType::R8, /*R8*/
		StackType::O,  /*O*/
		StackType::VT  /*VT*/
	};

	return types[(int) mt];
}


#define interp_ins_set_dreg(ins,dr) do { \
	ins->dreg = dr; \
} while (0)

#define interp_ins_set_sreg(ins,s1) do { \
	ins->sregs [0] = s1; \
} while (0)

#define interp_ins_set_sregs2(ins,s1,s2) do { \
	ins->sregs [0] = s1; \
	ins->sregs [1] = s2; \
} while (0)

#define interp_ins_set_sregs3(ins,s1,s2,s3) do { \
	ins->sregs [0] = s1; \
	ins->sregs [1] = s2; \
	ins->sregs [2] = s3; \
} while (0)

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

#define CHECK_STACK(n) \
	do { \
		int stack_size = sp - stack; \
		if (stack_size < (n)) \
			g_warning ("%s.%s: not enough values (%d < %d) on stack at %04x", \
				m_class_get_name (method->klass), method->name, \
				stack_size, n, ip - il_code); \
	} while (0)

/*
 * A token naming a type that will not load resolves to no class at all, and a
 * class that is merely broken resolves to one carrying the reason. Only the
 * second has a failure to report; asking the first for one dereferences null.
 */
#define CHECK_TYPELOAD(klass) \
	do { \
		if (!(klass)) { \
			mono_error_set_type_load_name (error, NULL, NULL, \
			                               "Could not load type from token 0x%08x", token); \
			goto exit; \
		} \
		if (mono_class_has_failure (klass)) { \
			mono_error_set_for_class_failure (error, klass); \
			goto exit; \
		} \
	} while (0)

#if NO_UNALIGNED_ACCESS
#define WRITE32(ip, v) \
	do { \
		* (ip) = * (guint16 *)(v); \
		* ((ip) + 1) = * ((guint16 *)(v) + 1); \
		(ip) += 2; \
	} while (0)

#define WRITE32_INS(ins, index, v) \
	do { \
		(ins)->data [index] = * (guint16 *)(v); \
		(ins)->data [index + 1] = * ((guint16 *)(v) + 1); \
	} while (0)

#define WRITE64(ins, v) \
	do { \
		*((ins) + 0) = * ((guint16 *)(v) + 0); \
		*((ins) + 1) = * ((guint16 *)(v) + 1); \
		*((ins) + 2) = * ((guint16 *)(v) + 2); \
		*((ins) + 3) = * ((guint16 *)(v) + 3); \
	} while (0)

#define WRITE64_INS(ins, index, v) \
	do { \
		(ins)->data [index] = * (guint16 *)(v); \
		(ins)->data [index + 1] = * ((guint16 *)(v) + 1); \
		(ins)->data [index + 2] = * ((guint16 *)(v) + 2); \
		(ins)->data [index + 3] = * ((guint16 *)(v) + 3); \
	} while (0)
#else
#define WRITE32(ip, v) \
	do { \
		* (guint32*)(ip) = * (guint32 *)(v); \
		(ip) += 2; \
	} while (0)
#define WRITE32_INS(ins, index, v) \
	do { \
		* (guint32 *)(&(ins)->data [index]) = * (guint32 *)(v); \
	} while (0)

#define WRITE64(ip, v) \
	do { \
		* (guint64*)(ip) = * (guint64 *)(v); \
		(ip) += 4; \
	} while (0)
#define WRITE64_INS(ins, index, v) \
	do { \
		* (guint64 *)(&(ins)->data [index]) = * (guint64 *)(v); \
	} while (0)

#endif

void
TransformData::realloc_stack ()
{
	int sppos = sp - stack;

	stack_capacity *= 2;
	stack = (StackInfo*) g_realloc (stack, stack_capacity * sizeof (stack [0]));
	sp = stack + sppos;
}

int
TransformData::get_tos_offset ()
{
	if (sp == stack)
		return 0;
	else
		return sp [-1].offset + sp [-1].size;
}

static MonoType*
get_type_from_stack (StackType type, MonoClass *klass)
{
	switch (type) {
		case StackType::I4: return m_class_get_byval_arg (mono_defaults.int32_class);
		case StackType::I8: return m_class_get_byval_arg (mono_defaults.int64_class);
		case StackType::R4: return m_class_get_byval_arg (mono_defaults.single_class);
		case StackType::R8: return m_class_get_byval_arg (mono_defaults.double_class);
		case StackType::O: return (klass && !m_class_is_valuetype (klass)) ? m_class_get_byval_arg (klass) : m_class_get_byval_arg (mono_defaults.object_class);
		case StackType::VT: return m_class_get_byval_arg (klass);
		case StackType::MP:
		case StackType::F:
			return m_class_get_byval_arg (mono_defaults.int_class);
		default:
			g_assert_not_reached ();
	}
}

/*
 * These are additional locals that can be allocated as we transform the code.
 * They are allocated past the method locals so they are accessed in the same
 * way, with an offset relative to the frame->locals.
 */
int
TransformData::create_interp_local_explicit (MonoType *type, int size)
{
	InterpLocal local{};

	local.type = type;
	local.mt = mint_type (type);
	local.offset = -1;
	local.size = size;

	locals.push_back (local);
	return (int) locals.size () - 1;
}

int
TransformData::create_interp_stack_local (StackType type, MonoClass *k, int type_size, int offset)
{
	int local = create_interp_local_explicit (get_type_from_stack (type, k), type_size);

	locals [local].flags |= INTERP_LOCAL_FLAG_EXECUTION_STACK;
	locals [local].stack_offset = offset;
	return local;
}

void
TransformData::push_type_explicit (StackType type, MonoClass *k, int type_size)
{
	int sp_height;
	sp_height = sp - stack + 1;
	if (sp_height > max_stack_height)
		max_stack_height = sp_height;
	if (sp_height > stack_capacity)
		realloc_stack ();
	sp->type = type;
	sp->klass = k;
	sp->flags = 0;
	sp->offset = get_tos_offset ();
	sp->local = create_interp_stack_local (type, k, type_size, sp->offset);
	sp->size = ALIGN_TO (type_size, MINT_STACK_SLOT_SIZE);
	if ((sp->size + sp->offset) > max_stack_size)
		max_stack_size = sp->size + sp->offset;
	sp++;
}

// This does not handle the size/offset of the entry. For those cases
// we need to manually pop the top of the stack and push a new entry.
#define SET_SIMPLE_TYPE(s, ty) \
	do { \
		g_assert (ty != StackType::VT); \
		g_assert ((s)->type != StackType::VT); \
		(s)->type = (ty); \
		(s)->flags = 0; \
		(s)->klass = NULL; \
	} while (0)

#define SET_TYPE(s, ty, k) \
	do { \
		g_assert (ty != StackType::VT); \
		g_assert ((s)->type != StackType::VT); \
		(s)->type = (ty); \
		(s)->flags = 0; \
		(s)->klass = k; \
	} while (0)

void
TransformData::set_type_and_local (StackInfo *sp, MonoClass *klass, StackType type)
{
	SET_TYPE (sp, type, klass);
	sp->local = create_interp_stack_local (type, NULL, MINT_STACK_SLOT_SIZE, sp->offset);
}

void
TransformData::set_simple_type_and_local (StackInfo *sp, StackType type)
{
	set_type_and_local (sp, NULL, type);
}

void
TransformData::push_type (StackType type, MonoClass *k)
{
	// We don't really care about the exact size for non-valuetypes
	push_type_explicit (type, k, MINT_STACK_SLOT_SIZE);
}

void
TransformData::push_simple_type (StackType type)
{
	push_type (type, NULL);
}

void
TransformData::push_type_vt (MonoClass *k, int size)
{
	push_type_explicit (StackType::VT, k, size);
}

void
TransformData::push_types (StackInfo *types, int count)
{
	for (int i = 0; i < count; i++)
		push_type_explicit (types [i].type, types [i].klass, types [i].size);
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


// Should be called when td->cbb branches to newbb and newbb can have a stack state
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
		coerce_fp (stack + i, newbb->stack_state [i].type);

		int sloc = stack [i].local;
		int dloc = newbb->stack_state [i].local;
		if (sloc != dloc) {
			MintType mt = locals [sloc].mt;
			int mov_op = get_mov_for_type (mt, FALSE);

			// FIXME can be hit in some IL cases. Should we merge the stack states ? (b41002.il)
			// g_assert (mov_op == get_mov_for_type (locals [dloc].mt, FALSE));

			interp_add_ins (mov_op);
			interp_ins_set_sreg (last_ins, stack [i].local);
			interp_ins_set_dreg (last_ins, newbb->stack_state [i].local);

			if (mt == MintType::VT) {
				g_assert (locals [sloc].size == locals [dloc].size);
				last_ins->data [0] = locals [sloc].size;
			}
		}
	}
}

// Initializes stack state at entry to bb, based on the current stack state
void
TransformData::init_bb_stack_state (InterpBasicBlock *bb)
{
	// FIXME If already initialized, then we need to generate mov to the registers in the state.
	// Check if already initialized
	if (bb->stack_height >= 0)
		return;

	bb->stack_height = sp - stack;
	if (bb->stack_height > 0) {
		int size = bb->stack_height * sizeof (stack [0]);
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

	InterpBasicBlock *target_bb = offset_to_bb [target];
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
	StackType type = sp [-1].type == StackType::O || sp [-1].type == StackType::MP ? StackType::I : sp [-1].type;
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

/*
 * Gives an int32 on the stack the width an eight-byte destination reads it at.
 *
 * The int32 wrote four bytes of the stack slot and the store takes all eight, so
 * the value needs an extension first. ECMA-335 Table III.9 names it for a call
 * argument, and the other assignments follow: a native unsigned int is filled
 * with zeroes and everything else with the sign. A C# compiler converts first,
 * so only hand-written IL gets here.
 */
void
TransformData::widen_i4_to_i8 (StackInfo *sp, MonoType *type)
{
	if (sp->type != StackType::I4 || mint_type (type) != MintType::I8)
		return;

	int op = mini_get_underlying_type (type)->type == MONO_TYPE_U ? MINT_CONV_I8_U4 : MINT_CONV_I8_I4;
	interp_add_conv (sp, NULL, StackType::I8, op);
}

/*
 * Gives a native int index the int32 form the indexing opcodes read.
 *
 * ECMA-335 III.4.8 lets an array index be an int32 or a native int, and switch
 * compares its operand as an unsigned integer. Both opcodes read four bytes, so
 * a wider index truncated there would select an element rather than fail the
 * bound test.
 */
void
TransformData::narrow_index (StackInfo *sp)
{
	if (sp->type == StackType::I8)
		interp_add_conv (sp, NULL, StackType::I4, MINT_CONV_INDEX_I8);
}

/*
 * Give the value at sp the precision a destination of type dtype holds it at.
 *
 * CIL has a single floating point stack type, so a value produced as r4 may
 * legally be stored where an r8 is kept and the other way round. The
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

/*
 * The stack type a value of this type is held at, for the floating point types
 * only. Anything else answers nothing, which coerce_fp leaves alone.
 */
static std::optional<StackType>
fp_stack_type (MonoType *type)
{
	if (type->byref)
		return std::nullopt;

	type = mini_get_underlying_type (type);

	switch (type->type) {
	case MONO_TYPE_R4: return StackType::R4;
	case MONO_TYPE_R8: return StackType::R8;
	default: return std::nullopt;
	}
}

void
TransformData::two_arg_branch (int mint_op, int offset, int inst_size)
{
	StackType type1 = sp [-1].type == StackType::O || sp [-1].type == StackType::MP ? StackType::I : sp [-1].type;
	StackType type2 = sp [-2].type == StackType::O || sp [-2].type == StackType::MP ? StackType::I : sp [-2].type;
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
		g_warning("%s.%s: branch type mismatch %d %d", 
			m_class_get_name (method->klass), method->name,
			(int) sp [-1].type, (int) sp [-2].type);
	}

	int long_op = op_for_stack_type (mint_op, type1);
	int short_op = long_op + MINT_BEQ_I4_S - MINT_BEQ_I4;
	sp -= 2;
	if (offset) {
		handle_branch (short_op, long_op, offset + inst_size);
		interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
	} else {
		interp_add_ins (MINT_NOP);
	}
}

void
TransformData::unary_arith_op (int mint_op)
{
	int op = op_for_stack_type (mint_op, sp [-1].type);
	CHECK_STACK (1);
	sp--;
	interp_add_ins (op);
	interp_ins_set_sreg (last_ins, sp [0].local);
	push_simple_type (sp [0].type);
	interp_ins_set_dreg (last_ins, sp [-1].local);
}

void
TransformData::binary_arith_op (int mint_op)
{
	StackType type1 = sp [-2].type;
	StackType type2 = sp [-1].type;
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
		g_warning("%s.%s: %04x arith type mismatch %s %d %d", 
			m_class_get_name (method->klass), method->name,
			ip - il_code, opname (mint_op), type1, type2);
	}
	op = op_for_stack_type (mint_op, type1);
	CHECK_STACK (2);
	sp -= 2;
	interp_add_ins (op);
	interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
	push_simple_type (type1);
	interp_ins_set_dreg (last_ins, sp [-1].local);
}

void
TransformData::shift_op (int mint_op)
{
	int op = op_for_stack_type (mint_op, sp [-2].type);
	CHECK_STACK (2);
	if (sp [-1].type != StackType::I4) {
		g_warning("%s.%s: shift type mismatch %d", 
			m_class_get_name (method->klass), method->name,
			sp [-2].type);
	}
	sp -= 2;
	interp_add_ins (op);
	interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
	push_simple_type (sp [0].type);
	interp_ins_set_dreg (last_ins, sp [-1].local);
}

static int
can_store (StackType st_value, StackType vt_value)
{
	if (st_value == StackType::O || st_value == StackType::MP)
		st_value = StackType::I;
	if (vt_value == StackType::O || vt_value == StackType::MP)
		vt_value = StackType::I;
	return st_value == vt_value;
}

MonoType*
TransformData::get_arg_type_exact (int n, MintType *mt)
{
	MonoType *type;
	gboolean hasthis = mono_method_signature_internal (method)->hasthis;

	if (hasthis && n == 0)
		type = m_class_get_byval_arg (method->klass);
	else
		type = mono_method_signature_internal (method)->params [n - !!hasthis];

	if (mt)
		*mt = mint_type (type);

	return type;
}

void
TransformData::load_arg (int n)
{
	gint32 size = 0;
	MintType mt;
	MonoClass *klass = NULL;
	MonoType *type;
	gboolean hasthis = mono_method_signature_internal (method)->hasthis;

	type = get_arg_type_exact (n, &mt);

	if (mt == MintType::VT) {
		klass = mono_class_from_mono_type_internal (type);
		if (mono_method_signature_internal (method)->pinvoke)
			size = mono_class_native_size (klass, NULL);
		else
			size = mono_class_value_size (klass, NULL);

		if (hasthis && n == 0) {
			mt = MintType::I;
			klass = NULL;
			push_type (stack_type_of (mt), klass);
		} else {
			g_assert (size < G_MAXUINT16);
			push_type_vt (klass, size);
		}
	} else {
		if ((hasthis || mt == MintType::I) && n == 0) {
			// Special case loading of the first ptr sized argument
			if (mt != MintType::O)
				mt = MintType::I;
		} else {
			if (mt == MintType::O)
				klass = mono_class_from_mono_type_internal (type);
		}
		push_type (stack_type_of (mt), klass);
	}
	interp_add_ins (get_mov_for_type (mt, TRUE));
	interp_ins_set_sreg (last_ins, n);
	interp_ins_set_dreg (last_ins, sp [-1].local); 
	if (mt == MintType::VT)
		last_ins->data [0] = size;
}

void
TransformData::store_arg (int n)
{
	gint32 size = 0;
	MintType mt;
	CHECK_STACK (1);
	MonoType *type;

	type = get_arg_type_exact (n, &mt);

	if (mt == MintType::VT) {
		MonoClass *klass = mono_class_from_mono_type_internal (type);
		if (mono_method_signature_internal (method)->pinvoke)
			size = mono_class_native_size (klass, NULL);
		else
			size = mono_class_value_size (klass, NULL);
		g_assert (size < G_MAXUINT16);
	}
	coerce_fp (sp - 1, stack_type_of (mt));
	--sp;
	interp_add_ins (get_mov_for_type (mt, FALSE));
	interp_ins_set_sreg (last_ins, sp [0].local);
	interp_ins_set_dreg (last_ins, n);
	if (mt == MintType::VT)
		last_ins->data [0] = size;
}

void
TransformData::load_local (int local)
{
	MintType mt = locals [local].mt;
	gint32 size = locals [local].size;
	MonoType *type = locals [local].type;

	if (mt == MintType::VT) {
		MonoClass *klass = mono_class_from_mono_type_internal (type);
		push_type_vt (klass, size);
	} else {
		MonoClass *klass = NULL;
		if (mt == MintType::O)
			klass = mono_class_from_mono_type_internal (type);
		push_type (stack_type_of (mt), klass);
	}
	interp_add_ins (get_mov_for_type (mt, TRUE));
	interp_ins_set_sreg (last_ins, local);
	interp_ins_set_dreg (last_ins, sp [-1].local);
	if (mt == MintType::VT)
		last_ins->data [0] = size;
}

void
TransformData::store_local (int local)
{
	MintType mt = locals [local].mt;
	CHECK_STACK (1);
#if SIZEOF_VOID_P == 8
	if (sp [-1].type == StackType::I4 && stack_type_of (mt) == StackType::I8)
		interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_I4);
#endif
	coerce_fp (sp - 1, stack_type_of (mt));
	if (!can_store(sp [-1].type, stack_type_of (mt))) {
		g_warning("%s.%s: Store local stack type mismatch %d %d", 
			m_class_get_name (method->klass), method->name,
			(int) stack_type_of (mt), (int) sp [-1].type);
	}
	--sp;
	interp_add_ins (get_mov_for_type (mt, FALSE));
	interp_ins_set_sreg (last_ins, sp [0].local);
	interp_ins_set_dreg (last_ins, local);
	if (mt == MintType::VT)
		last_ins->data [0] = locals [local].size;
}

guint16
TransformData::get_data_item_index_nonshared (void *ptr)
{
	data_items.push_back (ptr);
	return (guint16) (data_items.size () - 1);
}

guint16
TransformData::get_data_item_index (void *ptr)
{
	auto known = data_hash.find (ptr);

	if (known != data_hash.end ())
		return known->second;

	guint16 index = get_data_item_index_nonshared (ptr);

	data_hash[ptr] = index;
	return index;
}


static int mono_class_get_magic_index (MonoClass *k)
{
	if (mono_class_is_magic_int (k))
		return !strcmp ("nint", m_class_get_name (k)) ? 0 : 1;

	if (mono_class_is_magic_float (k))
		return 2;

	return -1;
}

void
TransformData::interp_generate_mae_throw (MonoMethod *method, MonoMethod *target_method)
{
	MonoJitICallInfo *info = &mono_get_jit_icall_info ()->mono_throw_method_access;

	/* Inject code throwing MethodAccessException */
	interp_add_ins (MINT_MONO_LDPTR);
	push_simple_type (StackType::I);
	interp_ins_set_dreg (last_ins, sp [-1].local);
	last_ins->data [0] = get_data_item_index (method);
	locals [sp [-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;

	interp_add_ins (MINT_MONO_LDPTR);
	push_simple_type (StackType::I);
	interp_ins_set_dreg (last_ins, sp [-1].local);
	last_ins->data [0] = get_data_item_index (target_method);
	locals [sp [-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;

	sp -= 2;
	interp_add_ins (MINT_ICALL_PP_V);
	interp_ins_set_dreg (last_ins, sp [0].local);
	last_ins->data [0] = get_data_item_index ((gpointer)info->func);

}

void
TransformData::interp_generate_bie_throw ()
{
	MonoJitICallInfo *info = &mono_get_jit_icall_info ()->mono_throw_bad_image;

	interp_add_ins (MINT_ICALL_V_V);
	// Allocate a dummy local to serve as dreg for this instruction
	push_simple_type (StackType::I4);
	sp--;
	interp_ins_set_dreg (last_ins, sp [0].local);
	last_ins->data [0] = get_data_item_index ((gpointer)info->func);
}

void
TransformData::interp_generate_not_supported_throw ()
{
	MonoJitICallInfo *info = &mono_get_jit_icall_info ()->mono_throw_not_supported;

	interp_add_ins (MINT_ICALL_V_V);
	// Allocate a dummy local to serve as dreg for this instruction
	push_simple_type (StackType::I4);
	sp--;
	interp_ins_set_dreg (last_ins, sp [0].local);
	last_ins->data [0] = get_data_item_index ((gpointer)info->func);
}

void
TransformData::interp_generate_ipe_throw_with_msg (MonoError *error_msg)
{
	MonoJitICallInfo *info = &mono_get_jit_icall_info ()->mono_throw_invalid_program;

	char *msg = mono_mem_manager_strdup (mem_manager, mono_error_get_message (error_msg));

	interp_add_ins (MINT_MONO_LDPTR);
	push_simple_type (StackType::I);
	interp_ins_set_dreg (last_ins, sp [-1].local);
	locals [sp [-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
	last_ins->data [0] = get_data_item_index (msg);

	sp -= 1;
	interp_add_ins (MINT_ICALL_P_V);
	interp_ins_set_dreg (last_ins, sp [0].local);
	last_ins->data [0] = get_data_item_index ((gpointer)info->func);
}

int
TransformData::create_interp_local (MonoType *type)
{
	int size, align;

	size = mono_type_size (type, &align);
	g_assert (align <= MINT_STACK_SLOT_SIZE);

	return create_interp_local_explicit (type, size);
}

int
TransformData::get_interp_local_offset (int local, gboolean resolve_stack_locals)
{
	// FIXME MINT_PROF_EXIT when void
	if (local == -1)
		return -1;

	if ((locals [local].flags & INTERP_LOCAL_FLAG_EXECUTION_STACK) && !resolve_stack_locals)
		return -1;

	if (locals [local].offset != -1)
		return locals [local].offset;

	if (locals [local].flags & INTERP_LOCAL_FLAG_EXECUTION_STACK) {
		locals [local].offset = total_locals_size + locals [local].stack_offset;
	} else {
		int size, offset;

		offset = total_locals_size;
		size = locals [local].size;

		locals [local].offset = offset;

		total_locals_size = ALIGN_TO (offset + size, MINT_STACK_SLOT_SIZE);
	}

	//g_assert (total_locals_size < G_MAXUINT16);

	return locals [local].offset;
}

/*
 * ins_offset is the associated offset of this instruction
 * if ins is null, it means the data belongs to an instruction that was
 * emitted in the final code
 * ip is the address where the arguments of the instruction are located
 */
static char*
dump_interp_ins_data (InterpInst *ins, gint32 ins_offset, const guint16 *data, guint16 opcode)
{
	GString *str = g_string_new ("");
	guint32 token;
	int target;

	switch (opargtype (opcode)) {
	case MintOpNoArgs:
		break;
	case MintOpUShortInt:
		g_string_append_printf (str, " %u", *(guint16*)data);
		break;
	case MintOpTwoShorts:
		g_string_append_printf (str, " %u,%u", *(guint16*)data, *(guint16 *)(data + 1));
		break;
	case MintOpShortAndInt:
		g_string_append_printf (str, " %u,%u", *(guint16*)data, (guint32)READ32(data + 1));
		break;
	case MintOpShortInt:
		g_string_append_printf (str, " %d", *(gint16*)data);
		break;
	case MintOpClassToken:
	case MintOpMethodToken:
	case MintOpFieldToken:
		token = * (guint16 *) data;
		g_string_append_printf (str, " %u", token);
		break;
	case MintOpInt:
		g_string_append_printf (str, " %d", (gint32)READ32 (data));
		break;
	case MintOpLongInt:
		g_string_append_printf (str, " %" PRId64, (gint64)READ64 (data));
		break;
	case MintOpFloat: {
		gint32 tmp = READ32 (data);
		g_string_append_printf (str, " %g", * (float *)&tmp);
		break;
	}
	case MintOpDouble: {
		gint64 tmp = READ64 (data);
		g_string_append_printf (str, " %g", * (double *)&tmp);
		break;
	}
	case MintOpShortBranch:
		if (ins) {
			/* the target IL is already embedded in the instruction */
			g_string_append_printf (str, " BB%d", ins->info.target_bb->index);
		} else {
			target = ins_offset + *(gint16*)data;
			g_string_append_printf (str, " IR_%04x", target);
		}
		break;
	case MintOpBranch:
		if (ins) {
			g_string_append_printf (str, " BB%d", ins->info.target_bb->index);
		} else {
			target = ins_offset + (gint32)READ32 (data);
			g_string_append_printf (str, " IR_%04x", target);
		}
		break;
	case MintOpSwitch: {
		int sval = (gint32)READ32 (data);
		int i;
		g_string_append_printf (str, "(");
		gint32 p = 2;
		for (i = 0; i < sval; ++i) {
			if (i > 0)
				g_string_append_printf (str, ", ");
			if (ins) {
				g_string_append_printf (str, "BB%d", ins->info.target_bb_table [i]->index);
			} else {
				g_string_append_printf (str, "IR_%04x", (gint32)READ32 (data + p));
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


static void
dump_interp_code (const guint16 *start, const guint16* end)
{
	const guint16 *p = start;
	while (p < end) {
		mono_interp_dis_mintop (p, start);
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
                        g_print (" %d", ins->sregs [i]);
                g_print ("],");
        } else {
                g_print (" nil],");
        }

	if (opcode == MINT_LDLOCA_S) {
		// LDLOCA has special semantics, it has data in sregs [0], but it doesn't have any sregs
		g_print (" %d", ins->sregs [0]);
	} else {
		char *descr = dump_interp_ins_data (ins, ins->il_offset, &ins->data [0], ins->opcode);
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


static MonoMethodHeader*
interp_method_get_header (MonoMethod* method, MonoError *error)
{
	/* An explanation: mono_method_get_header_internal returns an error if
	 * called on a method with no body (e.g. an abstract method, or an
	 * icall).  We don't want that.
	 */
	if (mono_method_has_no_body (method))
		return NULL;
	else
		return mono_method_get_header_internal (method, error);
}

/* stores top of stack as local and pushes address of it on stack */
void
TransformData::emit_store_value_as_local (MonoType *src)
{
	int local = create_interp_local (mini_native_type_replace_type (src));

	store_local (local);

	interp_add_ins (MINT_LDLOCA_S);
	push_simple_type (StackType::MP);
	interp_ins_set_dreg (last_ins, sp [-1].local);
	interp_ins_set_sreg (last_ins, local);
	locals [local].indirects++;
}

gboolean
TransformData::interp_ip_in_cbb (int il_offset)
{
	InterpBasicBlock *bb = offset_to_bb [il_offset];

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
	case MINT_LDC_I4_M1: return -1;
	case MINT_LDC_I4_0: return 0;
	case MINT_LDC_I4_1: return 1;
	case MINT_LDC_I4_2: return 2;
	case MINT_LDC_I4_3: return 3;
	case MINT_LDC_I4_4: return 4;
	case MINT_LDC_I4_5: return 5;
	case MINT_LDC_I4_6: return 6;
	case MINT_LDC_I4_7: return 7;
	case MINT_LDC_I4_8: return 8;
	case MINT_LDC_I4_S: return (gint32)(gint8)ins->data [0];
	case MINT_LDC_I4: return READ32 (&ins->data [0]);
	default:
		g_assert_not_reached ();
	}
}

/* If ins is not null, it will replace it with the ldc */
InterpInst*
TransformData::interp_get_ldc_i4_from_const (InterpInst *ins, gint32 ct, int dreg)
{
	int opcode;
	switch (ct) {
	case -1: opcode = MINT_LDC_I4_M1; break;
	case 0: opcode = MINT_LDC_I4_0; break;
	case 1: opcode = MINT_LDC_I4_1; break;
	case 2: opcode = MINT_LDC_I4_2; break;
	case 3: opcode = MINT_LDC_I4_3; break;
	case 4: opcode = MINT_LDC_I4_4; break;
	case 5: opcode = MINT_LDC_I4_5; break;
	case 6: opcode = MINT_LDC_I4_6; break;
	case 7: opcode = MINT_LDC_I4_7; break;
	case 8: opcode = MINT_LDC_I4_8; break;
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
		ins->data [0] = (gint8)ct;
	else if (new_size == 4)
		WRITE32_INS (ins, 0, &ct);

	return ins;
}

InterpInst*
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

static int
interp_get_ldind_for_mt (MintType mt)
{
	switch (mt) {
		case MintType::I1: return MINT_LDIND_I1_CHECK;
		case MintType::U1: return MINT_LDIND_U1_CHECK;
		case MintType::I2: return MINT_LDIND_I2_CHECK;
		case MintType::U2: return MINT_LDIND_U2_CHECK;
		case MintType::I4: return MINT_LDIND_I4_CHECK;
		case MintType::I8: return MINT_LDIND_I8_CHECK;
		case MintType::R4: return MINT_LDIND_R4_CHECK;
		case MintType::R8: return MINT_LDIND_R8_CHECK;
		case MintType::O: return MINT_LDIND_REF;
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
		interp_ins_set_sreg (last_ins, sp [0].local);
		push_type_vt (klass, size);
	} else {
		int opcode = interp_get_ldind_for_mt (mt);
		interp_add_ins (opcode);
		interp_ins_set_sreg (last_ins, sp [0].local);
		push_type (stack_type_of (mt), klass);
	}
	interp_ins_set_dreg (last_ins, sp [-1].local);
	if (mt == MintType::VT)
		last_ins->data [0] = size;
}

void
TransformData::interp_emit_stobj (MonoClass *klass)
{
	MintType mt = mint_type (m_class_get_byval_arg (klass));

	coerce_fp (sp - 1, stack_type_of (mt));

	if (mt == MintType::VT) {
		interp_add_ins (MINT_STOBJ_VT);
		last_ins->data [0] = get_data_item_index (klass);
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
			default: g_assert_not_reached (); break;
		}
		interp_add_ins (opcode);
	}
	sp -= 2;
	interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
}

void
TransformData::interp_emit_ldelema (MonoClass *array_class, MonoClass *check_class)
{
	MonoClass *element_class = m_class_get_element_class (array_class);
	int rank = m_class_get_rank (array_class);
	int size = mono_class_array_element_size (element_class);
	gboolean call_args = FALSE;

	gboolean bounded = m_class_get_byval_arg (array_class) ? m_class_get_byval_arg (array_class)->type == MONO_TYPE_ARRAY : FALSE;

	sp -= rank + 1;
	// We only need type checks when writing to array of references
	if (!check_class || m_class_is_valuetype (element_class)) {
		if (rank == 1 && !bounded) {
			interp_add_ins (MINT_LDELEMA1);
			interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
			g_assert (size < G_MAXUINT16);
			last_ins->data [0] = size;
		} else {
			interp_add_ins (MINT_LDELEMA);
			for (int i = 0; i < rank + 1; i++)
				locals [sp [i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
			last_ins->data [0] = rank;
			g_assert (size < G_MAXUINT16);
			last_ins->data [1] = size;
			call_args = TRUE;
		}
	} else {
		interp_add_ins (MINT_LDELEMA_TC);
		for (int i = 0; i < rank + 1; i++)
			locals [sp [i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
		last_ins->data [0] = get_data_item_index (check_class);
		call_args = TRUE;
	}

	push_simple_type (StackType::MP);
	interp_ins_set_dreg (last_ins, sp [-1].local);
	if (call_args)
		locals [sp [-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
}

gboolean
TransformData::interp_handle_magic_type_intrinsics (MonoMethod *target_method, MonoMethodSignature *csignature, int type_index)
{
	MonoClass *magic_class = target_method->klass;
	const char *tm = target_method->name;
	int i;

	const MintType mt = mint_type (m_class_get_byval_arg (magic_class));
	if (!strcmp (".ctor", tm)) {
		MonoType *arg = csignature->params [0];
		/* depending on SIZEOF_VOID_P and the type of the value passed to the .ctor we either have to CONV it, or do nothing */
		int arg_size = mini_magic_type_size (arg);

		if (arg_size > SIZEOF_VOID_P) { // 8 -> 4
			switch (type_index) {
			case 0: case 1:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I4_I8);
				break;
			case 2:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_R4_R8);
				break;
			}
		}

		if (arg_size < SIZEOF_VOID_P) { // 4 -> 8
			switch (type_index) {
			case 0:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_I4);
				break;
			case 1:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_U4);
				break;
			case 2:
				interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R8_R4);
				break;
			}
		}

		switch (type_index) {
		case 0: case 1:
#if SIZEOF_VOID_P == 4
			interp_add_ins (MINT_STIND_I4);
#else
			interp_add_ins (MINT_STIND_I8);
#endif
			break;
		case 2:
#if SIZEOF_VOID_P == 4
			interp_add_ins (MINT_STIND_R4);
#else
			interp_add_ins (MINT_STIND_R8);
#endif
			break;
		}
		sp -= 2;
		interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
		ip += 5;
		return TRUE;
	} else if (!strcmp ("op_Implicit", tm ) || !strcmp ("op_Explicit", tm)) {
		MonoType *src = csignature->params [0];
		MonoType *dst = csignature->ret;
		MonoClass *src_klass = mono_class_from_mono_type_internal (src);
		int src_size = mini_magic_type_size (src);
		int dst_size = mini_magic_type_size (dst);

		gboolean store_value_as_local = FALSE;

		switch (type_index) {
		case 0: case 1:
			if (!mini_magic_is_int_type (src) || !mini_magic_is_int_type (dst)) {
				if (mini_magic_is_int_type (src))
					store_value_as_local = TRUE;
				else if (mono_class_is_magic_float (src_klass))
					store_value_as_local = TRUE;
				else
					return FALSE;
			}
			break;
		case 2:
			if (!mini_magic_is_float_type (src) || !mini_magic_is_float_type (dst)) {
				if (mini_magic_is_float_type (src))
					store_value_as_local = TRUE;
				else if (mono_class_is_magic_int (src_klass))
					store_value_as_local = TRUE;
				else
					return FALSE;
			}
			break;
		}

		if (store_value_as_local) {
			emit_store_value_as_local (src);

			/* emit call to managed conversion method */
			return FALSE;
		}

		if (src_size > dst_size) { // 8 -> 4
			switch (type_index) {
			case 0: case 1:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I4_I8);
				break;
			case 2:
				interp_add_conv (sp - 1, NULL, StackType::R4, MINT_CONV_R4_R8);
				break;
			}
		}

		if (src_size < dst_size) { // 4 -> 8
			switch (type_index) {
			case 0:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_I4);
				break;
			case 1:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_U4);
				break;
			case 2:
				interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R8_R4);
				break;
			}
		}

		SET_TYPE (sp - 1, stack_type_of (mint_type (dst)), mono_class_from_mono_type_internal (dst));
		ip += 5;
		return TRUE;
	} else if (!strcmp ("op_Increment", tm)) {
		g_assert (type_index != 2); // no nfloat
#if SIZEOF_VOID_P == 8
		interp_add_ins (MINT_ADD1_I8);
#else
		interp_add_ins (MINT_ADD1_I4);
#endif
		sp--;
		interp_ins_set_sreg (last_ins, sp [0].local);
		push_type (stack_type_of (mt), magic_class);
		interp_ins_set_dreg (last_ins, sp [-1].local);
		ip += 5;
		return TRUE;
	} else if (!strcmp ("op_Decrement", tm)) {
		g_assert (type_index != 2); // no nfloat
#if SIZEOF_VOID_P == 8
		interp_add_ins (MINT_SUB1_I8);
#else
		interp_add_ins (MINT_SUB1_I4);
#endif
		sp--;
		interp_ins_set_sreg (last_ins, sp [0].local);
		push_type (stack_type_of (mt), magic_class);
		interp_ins_set_dreg (last_ins, sp [-1].local);
		ip += 5;
		return TRUE;
	} else if (!strcmp ("CompareTo", tm) || !strcmp ("Equals", tm)) {
		MonoType *arg = csignature->params [0];
		MintType mt = mint_type (arg);

		/* on 'System.n*::{CompareTo,Equals} (System.n*)' variant we need to push managed
		 * pointer instead of value */
		if (mt != MintType::O)
			emit_store_value_as_local (arg);

		/* emit call to managed conversion method */
		return FALSE;
	} else if (!strcmp (".cctor", tm)) {
		return FALSE;
	} else if (!strcmp ("Parse", tm)) {
		return FALSE;
	} else if (!strcmp ("ToString", tm)) {
		return FALSE;
	} else if (!strcmp ("GetHashCode", tm)) {
		return FALSE;
	} else if (!strcmp ("IsNaN", tm) || !strcmp ("IsInfinity", tm) || !strcmp ("IsNegativeInfinity", tm) || !strcmp ("IsPositiveInfinity", tm)) {
		g_assert (type_index == 2); // nfloat only
		return FALSE;
	}

	for (i = 0; i < sizeof (int_unnop) / sizeof  (MagicIntrinsic); ++i) {
		if (!strcmp (int_unnop [i].op_name, tm)) {
			interp_add_ins (int_unnop [i].insn [type_index]);
			sp--;
			interp_ins_set_sreg (last_ins, sp [0].local);
			push_type (stack_type_of (mt), magic_class);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 5;
			return TRUE;
		}
	}

	for (i = 0; i < sizeof (int_binop) / sizeof  (MagicIntrinsic); ++i) {
		if (!strcmp (int_binop [i].op_name, tm)) {
			interp_add_ins (int_binop [i].insn [type_index]);
			sp -= 2;
			interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
			push_type (stack_type_of (mt), magic_class);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 5;
			return TRUE;
		}
	}

	for (i = 0; i < sizeof (int_cmpop) / sizeof  (MagicIntrinsic); ++i) {
		if (!strcmp (int_cmpop [i].op_name, tm)) {
			MonoClass *k = mono_defaults.boolean_class;
			interp_add_ins (int_cmpop [i].insn [type_index]);
			sp -= 2;
			interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
			push_type (stack_type_of (mint_type (m_class_get_byval_arg (k))), k);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 5;
			return TRUE;
		}
	}

	g_error ("TODO: interp_transform_call %s:%s", m_class_get_name (target_method->klass), tm);
}

/* Return TRUE if call transformation is finished */
gboolean
TransformData::interp_handle_intrinsics (MonoMethod *target_method, MonoClass *constrained_class, MonoMethodSignature *csignature, gboolean readonly, int *op)
{
	const char *tm = target_method->name;
	int type_index = mono_class_get_magic_index (target_method->klass);
	gboolean in_corlib = m_class_get_image (target_method->klass) == mono_defaults.corlib;
	const char *klass_name_space;
	if (m_class_get_nested_in (target_method->klass))
		klass_name_space = m_class_get_name_space (m_class_get_nested_in (target_method->klass));
	else
		klass_name_space = m_class_get_name_space (target_method->klass);
	const char *klass_name = m_class_get_name (target_method->klass);

	if (target_method->klass == mono_defaults.string_class) {
		if (tm [0] == 'g') {
			if (strcmp (tm, "get_Chars") == 0)
				*op = MINT_GETCHR;
			else if (strcmp (tm, "get_Length") == 0)
				*op = MINT_STRLEN;
		}
	} else if (type_index >= 0) {
		return interp_handle_magic_type_intrinsics (target_method, csignature, type_index);
	} else if (mono_class_is_subclass_of_internal (target_method->klass, mono_defaults.array_class, FALSE)) {
		if (!strcmp (tm, "get_Rank")) {
			*op = MINT_ARRAY_RANK;
		} else if (!strcmp (tm, "get_Length")) {
			*op = MINT_LDLEN;
		} else if (!strcmp (tm, "GetElementSize")) {
			*op = MINT_ARRAY_ELEMENT_SIZE;
		} else if (!strcmp (tm, "IsPrimitive")) {
			*op = MINT_ARRAY_IS_PRIMITIVE;
		} else if (!strcmp (tm, "Address")) {
			MonoClass *check_class = readonly ? NULL : m_class_get_element_class (target_method->klass);
			interp_emit_ldelema (target_method->klass, check_class);
			ip += 5;
			return TRUE;
#ifndef ENABLE_NETCORE
		} else if (!strcmp (tm, "UnsafeMov") || !strcmp (tm, "UnsafeLoad")) {
			*op = MINT_CALLRUN;
#endif
		} else if (!strcmp (tm, "Get")) {
			interp_emit_ldelema (target_method->klass, NULL);
			interp_emit_ldobj (m_class_get_element_class (target_method->klass));
			ip += 5;
			return TRUE;
		} else if (!strcmp (tm, "Set")) {
			MonoClass *element_class = m_class_get_element_class (target_method->klass);
			MonoType *local_type = m_class_get_byval_arg (element_class);
			MonoClass *value_class = sp [-1].klass;
			// If value_class is NULL it means the top of stack is a simple type (valuetype)
			// which doesn't require type checks, or that we have no type information because
			// the code is unsafe (like in some wrappers). In that case we assume the type
			// of the array and don't do any checks.

			int local = create_interp_local (local_type);

			store_local (local);
			interp_emit_ldelema (target_method->klass, value_class);
			load_local (local);
			interp_emit_stobj (element_class);
			ip += 5;
			return TRUE;
		} else if (!strcmp (tm, "UnsafeStore")) {
			g_error ("TODO ArrayClass::UnsafeStore");
		}
	} else if (in_corlib &&
			!strcmp (klass_name_space, "System.Diagnostics") &&
			!strcmp (klass_name, "Debugger")) {
		if (!strcmp (tm, "Break") && csignature->param_count == 0) {
			if (mini_should_insert_breakpoint (method))
				*op = MINT_BREAK;
		}
	} else if (in_corlib &&
			!strcmp (klass_name_space, "System") &&
			!strcmp (klass_name, "SpanHelpers") &&
			!strcmp (tm, "ClearWithReferences")) {
		*op = MINT_INTRINS_CLEAR_WITH_REFERENCES;
	} else if (in_corlib && !strcmp (klass_name_space, "System") && !strcmp (klass_name, "ByReference`1")) {
		g_assert (!strcmp (tm, "get_Value"));
		*op = MINT_INTRINS_BYREFERENCE_GET_VALUE;
	} else if (in_corlib && !strcmp (klass_name_space, "System") && !strcmp (klass_name, "Marvin")) {
		if (!strcmp (tm, "Block"))
			*op = MINT_INTRINS_MARVIN_BLOCK;
	} else if (in_corlib && !strcmp (klass_name_space, "System.Runtime.InteropServices") && !strcmp (klass_name, "MemoryMarshal")) {
		if (!strcmp (tm, "GetArrayDataReference"))
			*op = MINT_INTRINS_MEMORYMARSHAL_GETARRAYDATAREF;
	} else if (in_corlib && !strcmp (klass_name_space, "System.Text.Unicode") && !strcmp (klass_name, "Utf16Utility")) {
		if (!strcmp (tm, "ConvertAllAsciiCharsInUInt32ToUppercase"))
			*op = MINT_INTRINS_ASCII_CHARS_TO_UPPERCASE;
		else if (!strcmp (tm, "UInt32OrdinalIgnoreCaseAscii"))
			*op = MINT_INTRINS_ORDINAL_IGNORE_CASE_ASCII;
		else if (!strcmp (tm, "UInt64OrdinalIgnoreCaseAscii"))
			*op = MINT_INTRINS_64ORDINAL_IGNORE_CASE_ASCII;
	} else if (in_corlib && !strcmp (klass_name_space, "System.Text") && !strcmp (klass_name, "ASCIIUtility")) {
		if (!strcmp (tm, "WidenAsciiToUtf16"))
			*op = MINT_INTRINS_WIDEN_ASCII_TO_UTF16;
	} else if (in_corlib && !strcmp (klass_name_space, "System") && !strcmp (klass_name, "Number")) {
		if (!strcmp (tm, "UInt32ToDecStr") && csignature->param_count == 1) {
			ERROR_DECL(error);
			MonoVTable *vtable = mono_class_vtable_checked (rtm->domain, target_method->klass, error);
			if (!is_ok (error)) {
				mono_interp_error_cleanup (error);
				return FALSE;
			}
			/* Don't use intrinsic if cctor not yet run */
			if (!vtable->initialized)
				return FALSE;
			/* The cache is the first static field. Update this if bcl code changes */
			MonoClassField *field = m_class_get_fields (target_method->klass);
			g_assert (!strcmp (field->name, "s_singleDigitStringCache"));
			interp_add_ins (MINT_INTRINS_U32_TO_DECSTR);
			last_ins->data [0] = get_data_item_index ((char*)mono_vtable_get_static_field_data (vtable) + field->offset);
			last_ins->data [1] = get_data_item_index (mono_class_vtable_checked (rtm->domain, mono_defaults.string_class, error));
			sp--;
			interp_ins_set_sreg (last_ins, sp [0].local);
			push_type (StackType::O, mono_defaults.string_class);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 5;
			return TRUE;
		}
	} else if (in_corlib && !strcmp (klass_name_space, "System") &&
			(!strcmp (klass_name, "Math") || !strcmp (klass_name, "MathF"))) {
		gboolean is_float = strcmp (klass_name, "MathF") == 0;
		int param_type = is_float ? MONO_TYPE_R4 : MONO_TYPE_R8;
		// FIXME add also intrinsic for Round
		if (csignature->param_count == 1 && csignature->params [0]->type == param_type) {
			// unops
			if (tm [0] == 'A') {
				if (strcmp (tm, "Abs") == 0) {
					*op = MINT_ABS;
				} else if (strcmp (tm, "Asin") == 0){
					*op = MINT_ASIN;
				} else if (strcmp (tm, "Asinh") == 0){
					*op = MINT_ASINH;
				} else if (strcmp (tm, "Acos") == 0){
					*op = MINT_ACOS;
				} else if (strcmp (tm, "Acosh") == 0){
					*op = MINT_ACOSH;
				} else if (strcmp (tm, "Atan") == 0){
					*op = MINT_ATAN;
				} else if (strcmp (tm, "Atanh") == 0){
					*op = MINT_ATANH;
				}
			} else if (tm [0] == 'C') {
				if (strcmp (tm, "Ceiling") == 0) {
					*op = MINT_CEILING;
				} else if (strcmp (tm, "Cos") == 0) {
					*op = MINT_COS;
				} else if (strcmp (tm, "Cbrt") == 0){
					*op = MINT_CBRT;
				} else if (strcmp (tm, "Cosh") == 0){
					*op = MINT_COSH;
				}
			} else if (strcmp (tm, "Exp") == 0) {
				*op = MINT_EXP;
			} else if (strcmp (tm, "Floor") == 0) {
				*op = MINT_FLOOR;
			} else if (strcmp (tm, "ILogB") == 0) {
				*op = MINT_ILOGB;
			} else if (tm [0] == 'L') {
				if (strcmp (tm, "Log") == 0) {
					*op = MINT_LOG;
				} else if (strcmp (tm, "Log2") == 0) {
					*op = MINT_LOG2;
				} else if (strcmp (tm, "Log10") == 0) {
					*op = MINT_LOG10;
				}
			} else if (tm [0] == 'S') {
				if (strcmp (tm, "Sin") == 0) {
					*op = MINT_SIN;
				} else if (strcmp (tm, "Sqrt") == 0) {
					*op = MINT_SQRT;
				} else if (strcmp (tm, "Sinh") == 0){
					*op = MINT_SINH;
				}
			} else if (tm [0] == 'T') {
				if (strcmp (tm, "Tan") == 0) {
					*op = MINT_TAN;
				} else if (strcmp (tm, "Tanh") == 0){
					*op = MINT_TANH;
				}
			}
		} else if (csignature->param_count == 2 && csignature->params [0]->type == param_type && csignature->params [1]->type == param_type) {
			if (strcmp (tm, "Atan2") == 0)
				*op = MINT_ATAN2;
			else if (strcmp (tm, "Pow") == 0)
				*op = MINT_POW;
		} else if (csignature->param_count == 3 && csignature->params [0]->type == param_type && csignature->params [1]->type == param_type && csignature->params [2]->type == param_type) {
			if (strcmp (tm, "FusedMultiplyAdd") == 0)
				*op = MINT_FMA;
		} else if (csignature->param_count == 2 && csignature->params [0]->type == param_type && csignature->params [1]->type == MONO_TYPE_I4 && strcmp (tm, "ScaleB") == 0) {
			*op = MINT_SCALEB;
		}

		if (*op != -1 && is_float) {
			*op = *op + (MINT_ABSF - MINT_ABS);
		}
	} else if (in_corlib && !strcmp (klass_name_space, "System") && (!strcmp (klass_name, "Span`1") || !strcmp (klass_name, "ReadOnlySpan`1"))) {
		if (!strcmp (tm, "get_Item")) {
			MonoGenericClass *gclass = mono_class_get_generic_class (target_method->klass);
			MonoClass *param_class = mono_class_from_mono_type_internal (gclass->context.class_inst->type_argv [0]);

			if (!mini_is_gsharedvt_variable_klass (param_class)) {
				MonoClassField *length_field = mono_class_get_field_from_name_full (target_method->klass, "_length", NULL);
				g_assert (length_field);
				int offset_length = length_field->offset - sizeof (MonoObject);

				MonoClassField *ptr_field = mono_class_get_field_from_name_full (target_method->klass, "_pointer", NULL);
				g_assert (ptr_field);
				int offset_pointer = ptr_field->offset - sizeof (MonoObject);

				int size = mono_class_array_element_size (param_class);
				interp_add_ins (MINT_GETITEM_SPAN);
				last_ins->data [0] = size;
				last_ins->data [1] = offset_length;
				last_ins->data [2] = offset_pointer;

				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				ip += 5;
				return TRUE;
			}
		} else if (!strcmp (tm, "get_Length")) {
			MonoClassField *length_field = mono_class_get_field_from_name_full (target_method->klass, "_length", NULL);
			g_assert (length_field);
			int offset_length = length_field->offset - sizeof (MonoObject);
			interp_add_ins (MINT_LDLEN_SPAN);
			last_ins->data [0] = offset_length;
			sp--;
			interp_ins_set_sreg (last_ins, sp [0].local);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 5;
			return TRUE;
		}
	} else if (((in_corlib && !strcmp (klass_name_space, "Internal.Runtime.CompilerServices"))
				|| !strcmp (klass_name_space, "System.Runtime.CompilerServices"))
			   && !strcmp (klass_name, "Unsafe")) {
#ifdef ENABLE_NETCORE
		if (!strcmp (tm, "AddByteOffset"))
			*op = MINT_INTRINS_UNSAFE_ADD_BYTE_OFFSET;
		else if (!strcmp (tm, "ByteOffset"))
			*op = MINT_INTRINS_UNSAFE_BYTE_OFFSET;
		else if (!strcmp (tm, "As") || !strcmp (tm, "AsRef"))
			*op = MINT_MOV_P;
		else if (!strcmp (tm, "AsPointer")) {
			/* NOP */
			SET_SIMPLE_TYPE (sp - 1, StackType::MP);
			ip += 5;
			return TRUE;
		} else if (!strcmp (tm, "IsAddressLessThan")) {
			MonoGenericContext *ctx = mono_method_get_context (target_method);
			g_assert (ctx);
			g_assert (ctx->method_inst);
			g_assert (ctx->method_inst->type_argc == 1);

			MonoClass *k = mono_defaults.boolean_class;
			interp_add_ins (MINT_CLT_UN_P);
			sp -= 2;
			interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
			push_type (stack_type_of (mint_type (m_class_get_byval_arg (k))), k);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 5;
			return TRUE;
		} else if (!strcmp (tm, "SizeOf")) {
			MonoGenericContext *ctx = mono_method_get_context (target_method);
			g_assert (ctx);
			g_assert (ctx->method_inst);
			g_assert (ctx->method_inst->type_argc == 1);
			MonoType *t = ctx->method_inst->type_argv [0];
			int align;
			int esize = mono_type_size (t, &align);
			interp_add_ins (MINT_LDC_I4);
			WRITE32_INS (last_ins, 0, &esize);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 5;
			return TRUE;
		} else if (!strcmp (tm, "AreSame")) {
			*op = MINT_CEQ_P;
		} else if (!strcmp (tm, "SkipInit")) {
			*op = MINT_NOP;
		} else if (!strcmp (tm, "InitBlockUnaligned")) {
			*op = MINT_INITBLK;
		}
#endif
	} else if (in_corlib && !strcmp (klass_name_space, "System.Runtime.CompilerServices") && !strcmp (klass_name, "RuntimeHelpers")) {
#ifdef ENABLE_NETCORE
		if (!strcmp (tm, "get_OffsetToStringData")) {
			g_assert (csignature->param_count == 0);
			int offset = MONO_STRUCT_OFFSET (MonoString, chars);
			interp_add_ins (MINT_LDC_I4);
			WRITE32_INS (last_ins, 0, &offset);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 5;
			return TRUE;
		} else if (!strcmp (tm, "GetRawData")) {
			interp_add_ins (MINT_LDFLDA_UNSAFE);
			last_ins->data [0] = (gint16) MONO_ABI_SIZEOF (MonoObject);

			sp--;
			interp_ins_set_sreg (last_ins, sp [0].local);
			push_simple_type (StackType::MP);
			interp_ins_set_dreg (last_ins, sp [-1].local);

			ip += 5;
			return TRUE;
		} else if (!strcmp (tm, "IsBitwiseEquatable")) {
			g_assert (csignature->param_count == 0);
			MonoGenericContext *ctx = mono_method_get_context (target_method);
			g_assert (ctx);
			g_assert (ctx->method_inst);
			g_assert (ctx->method_inst->type_argc == 1);
			MonoType *t = mini_get_underlying_type (ctx->method_inst->type_argv [0]);

			if (MONO_TYPE_IS_PRIMITIVE (t) && t->type != MONO_TYPE_R4 && t->type != MONO_TYPE_R8)
				*op = MINT_LDC_I4_1;
			else
				*op = MINT_LDC_I4_0;
		} else if (!strcmp (tm, "ObjectHasComponentSize")) {
			*op = MINT_INTRINS_RUNTIMEHELPERS_OBJECT_HAS_COMPONENT_SIZE;
		} else if (!strcmp (tm, "IsReferenceOrContainsReferences")) {
			g_assert (csignature->param_count == 0);
			MonoGenericContext *ctx = mono_method_get_context (target_method);
			g_assert (ctx);
			g_assert (ctx->method_inst);
			g_assert (ctx->method_inst->type_argc == 1);
			MonoType *t = mini_get_underlying_type (ctx->method_inst->type_argv [0]);

			gboolean has_refs;

			MonoClass *klass = mono_class_from_mono_type_internal (t);
			mono_class_init_internal (klass);
			if (MONO_TYPE_IS_REFERENCE (t))
				has_refs = TRUE;
			else if (MONO_TYPE_IS_PRIMITIVE (t))
				has_refs = FALSE;
			else
				has_refs = m_class_has_references (klass);

			*op = has_refs ? MINT_LDC_I4_1 : MINT_LDC_I4_0;
		}
#endif
	} else if (in_corlib && !strcmp (klass_name_space, "System") && !strcmp (klass_name, "RuntimeMethodHandle") && !strcmp (tm, "GetFunctionPointer") && csignature->param_count == 1) {
		// Same answer as the icall, without transforming the method to get it.
		*op = MINT_LDFTN_DYNAMIC;
	} else if (in_corlib && target_method->klass == mono_defaults.systemtype_class && !strcmp (target_method->name, "op_Equality")) {
		*op = MINT_CEQ_P;
	} else if (in_corlib && target_method->klass == mono_defaults.object_class) {
		if (!strcmp (tm, "InternalGetHashCode"))
			*op = MINT_INTRINS_GET_HASHCODE;
		else if (!strcmp (tm, "GetType")
#ifndef DISABLE_REMOTING
			// Invoking GetType via reflection on proxies has some special semantics
			// See InterfaceProxyGetTypeViaReflectionOkay corlib test
			&& method->wrapper_type != MONO_WRAPPER_RUNTIME_INVOKE
#endif
				)
			*op = MINT_INTRINS_GET_TYPE;
	} else if (in_corlib && target_method->klass == mono_defaults.enum_class && !strcmp (tm, "HasFlag")) {
		gboolean intrinsify = FALSE;
		MonoClass *base_klass = NULL;
		if (last_ins && last_ins->opcode == MINT_BOX &&
				last_ins->prev && interp_ins_is_ldc (last_ins->prev) &&
				last_ins->prev->prev && last_ins->prev->prev->opcode == MINT_BOX &&
				sp [-2].klass == sp [-1].klass &&
				interp_ip_in_cbb (ip - il_code)) {
			// csc pattern : box, ldc, box, call HasFlag
			g_assert (m_class_is_enumtype (sp [-2].klass));
			MonoType *base_type = mono_type_get_underlying_type (m_class_get_byval_arg (sp [-2].klass));
			base_klass = mono_class_from_mono_type_internal (base_type);

			// Remove the boxing of valuetypes, by replacing them with moves
			last_ins->prev->prev->opcode = get_mov_for_type (mint_type (base_type), FALSE);
			last_ins->opcode = get_mov_for_type (mint_type (base_type), FALSE);

			intrinsify = TRUE;
		} else if (last_ins && last_ins->opcode == MINT_BOX &&
				last_ins->prev && interp_ins_is_ldc (last_ins->prev) &&
				constrained_class && sp [-1].klass == constrained_class &&
				interp_ip_in_cbb (ip - il_code)) {
			// mcs pattern : ldc, box, constrained Enum, call HasFlag
			g_assert (m_class_is_enumtype (constrained_class));
			MonoType *base_type = mono_type_get_underlying_type (m_class_get_byval_arg (constrained_class));
			base_klass = mono_class_from_mono_type_internal (base_type);
			MintType mt = mint_type (m_class_get_byval_arg (base_klass));

			// Remove boxing and load the value of this
			last_ins->opcode = get_mov_for_type (mt, FALSE);
			InterpInst *ins = interp_insert_ins (last_ins->prev->prev, interp_get_ldind_for_mt (mt));
			interp_ins_set_sreg (ins, sp [-2].local);
			interp_ins_set_dreg (ins, sp [-2].local);
			intrinsify = TRUE;
		}
		if (intrinsify) {
			interp_add_ins (MINT_INTRINS_ENUM_HASFLAG);
			last_ins->data [0] = get_data_item_index (base_klass);
			sp -= 2;
			interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 5;
			return TRUE;
		}
	} else if (in_corlib && !strcmp (klass_name_space, "System.Threading") && !strcmp (klass_name, "Interlocked")) {
		if (!strcmp (tm, "MemoryBarrier") && csignature->param_count == 0)
			*op = MINT_MONO_MEMORY_BARRIER;
		else if (!strcmp (tm, "Exchange") && csignature->param_count == 2 && csignature->params [0]->type == MONO_TYPE_I8 && csignature->params [1]->type == MONO_TYPE_I8)
			*op = MINT_MONO_EXCHANGE_I8;
	} else if (in_corlib && !strcmp (klass_name_space, "System.Threading") && !strcmp (klass_name, "Thread")) {
		if (!strcmp (tm, "MemoryBarrier") && csignature->param_count == 0)
			*op = MINT_MONO_MEMORY_BARRIER;
	} else if (in_corlib &&
			!strcmp (klass_name_space, "System.Runtime.CompilerServices") &&
			!strcmp (klass_name, "JitHelpers") &&
			(!strcmp (tm, "EnumEquals") || !strcmp (tm, "EnumCompareTo"))) {
		MonoGenericContext *ctx = mono_method_get_context (target_method);
		g_assert (ctx);
		g_assert (ctx->method_inst);
		g_assert (ctx->method_inst->type_argc == 1);
		g_assert (csignature->param_count == 2);

		MonoType *t = ctx->method_inst->type_argv [0];
		t = mini_get_underlying_type (t);

		gboolean is_i8 = (t->type == MONO_TYPE_I8 || t->type == MONO_TYPE_U8);
		gboolean is_unsigned = (t->type == MONO_TYPE_U1 || t->type == MONO_TYPE_U2 || t->type == MONO_TYPE_U4 || t->type == MONO_TYPE_U8 || t->type == MONO_TYPE_U);

		gboolean is_compareto = strcmp (tm, "EnumCompareTo") == 0;
		if (is_compareto) {
			int locala, localb;
			locala = create_interp_local (t);
			localb = create_interp_local (t);

			// Save arguments
			store_local (localb);
			store_local (locala);
			// (a > b)
			load_local (locala);
			load_local (localb);
			if (is_unsigned)
				interp_add_ins (is_i8 ? MINT_CGT_UN_I8 : MINT_CGT_UN_I4);
			else
				interp_add_ins (is_i8 ? MINT_CGT_I8 : MINT_CGT_I4);
			sp -= 2;
			interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			// (a < b)
			load_local (locala);
			load_local (localb);
			if (is_unsigned)
				interp_add_ins (is_i8 ? MINT_CLT_UN_I8 : MINT_CLT_UN_I4);
			else
				interp_add_ins (is_i8 ? MINT_CLT_I8 : MINT_CLT_I4);
			sp -= 2;
			interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			// (a > b) - (a < b)
			interp_add_ins (MINT_SUB_I4);
			sp -= 2;
			interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 5;
			return TRUE;
		} else {
			if (is_i8) {
				*op = MINT_CEQ_I8;
			} else {
				*op = MINT_CEQ_I4;
			}
		}
	}
#ifdef ENABLE_NETCORE
	else if (in_corlib &&
			   !strcmp ("System.Runtime.CompilerServices", klass_name_space) &&
			   !strcmp ("RuntimeFeature", klass_name)) {
		if (!strcmp (tm, "get_IsDynamicCodeSupported"))
			*op = MINT_LDC_I4_1;
		else if (!strcmp (tm, "get_IsDynamicCodeCompiled"))
			*op = MINT_LDC_I4_0;
	} else if (in_corlib &&
			!strncmp ("System.Runtime.Intrinsics", klass_name_space, 25) &&
			!strcmp (tm, "get_IsSupported")) {
		*op = MINT_LDC_I4_0;
	}
#endif

	return FALSE;
}

static MonoMethod*
interp_transform_internal_calls (MonoMethod *method, MonoMethod *target_method, MonoMethodSignature *csignature, gboolean is_virtual)
{
	if (((method->wrapper_type == MONO_WRAPPER_NONE) || (method->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD)) && target_method != NULL) {
		if (target_method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)
			target_method = mono_marshal_get_native_wrapper (target_method, FALSE, FALSE);
		if (!is_virtual && target_method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
			target_method = mono_marshal_get_synchronized_wrapper (target_method);

		if (target_method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL && !is_virtual && !mono_class_is_marshalbyref (target_method->klass) && m_class_get_rank (target_method->klass) == 0)
			target_method = mono_marshal_get_native_wrapper (target_method, FALSE, FALSE);
	}
	return target_method;
}

static gboolean
interp_type_as_ptr (MonoType *tp)
{
	if (MONO_TYPE_IS_POINTER (tp))
		return TRUE;
	if (MONO_TYPE_IS_REFERENCE (tp))
		return TRUE;
	if ((tp)->type == MONO_TYPE_I4)
		return TRUE;
#if SIZEOF_VOID_P == 8
	if ((tp)->type == MONO_TYPE_I8)
		return TRUE;
#endif
	if ((tp)->type == MONO_TYPE_BOOLEAN)
		return TRUE;
	if ((tp)->type == MONO_TYPE_CHAR)
		return TRUE;
	if ((tp)->type == MONO_TYPE_VALUETYPE && m_class_is_enumtype (tp->data.klass))
		return TRUE;
	return FALSE;
}

#define INTERP_TYPE_AS_PTR(tp) interp_type_as_ptr (tp)

static int
interp_icall_op_for_sig (MonoMethodSignature *sig)
{
	int op = -1;
	switch (sig->param_count) {
	case 0:
		if (MONO_TYPE_IS_VOID (sig->ret))
			op = MINT_ICALL_V_V;
		else if (INTERP_TYPE_AS_PTR (sig->ret))
			op = MINT_ICALL_V_P;
		break;
	case 1:
		if (MONO_TYPE_IS_VOID (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params [0]))
				op = MINT_ICALL_P_V;
		} else if (INTERP_TYPE_AS_PTR (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params [0]))
				op = MINT_ICALL_P_P;
		}
		break;
	case 2:
		if (MONO_TYPE_IS_VOID (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params [0]) &&
					INTERP_TYPE_AS_PTR (sig->params [1]))
				op = MINT_ICALL_PP_V;
		} else if (INTERP_TYPE_AS_PTR (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params [0]) &&
					INTERP_TYPE_AS_PTR (sig->params [1]))
				op = MINT_ICALL_PP_P;
		}
		break;
	case 3:
		if (MONO_TYPE_IS_VOID (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params [0]) &&
					INTERP_TYPE_AS_PTR (sig->params [1]) &&
					INTERP_TYPE_AS_PTR (sig->params [2]))
				op = MINT_ICALL_PPP_V;
		} else if (INTERP_TYPE_AS_PTR (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params [0]) &&
					INTERP_TYPE_AS_PTR (sig->params [1]) &&
					INTERP_TYPE_AS_PTR (sig->params [2]))
				op = MINT_ICALL_PPP_P;
		}
		break;
	case 4:
		if (MONO_TYPE_IS_VOID (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params [0]) &&
					INTERP_TYPE_AS_PTR (sig->params [1]) &&
					INTERP_TYPE_AS_PTR (sig->params [2]) &&
					INTERP_TYPE_AS_PTR (sig->params [3]))
				op = MINT_ICALL_PPPP_V;
		} else if (INTERP_TYPE_AS_PTR (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params [0]) &&
					INTERP_TYPE_AS_PTR (sig->params [1]) &&
					INTERP_TYPE_AS_PTR (sig->params [2]) &&
					INTERP_TYPE_AS_PTR (sig->params [3]))
				op = MINT_ICALL_PPPP_P;
		}
		break;
	case 5:
		if (MONO_TYPE_IS_VOID (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params [0]) &&
					INTERP_TYPE_AS_PTR (sig->params [1]) &&
					INTERP_TYPE_AS_PTR (sig->params [2]) &&
					INTERP_TYPE_AS_PTR (sig->params [3]) &&
					INTERP_TYPE_AS_PTR (sig->params [4]))
				op = MINT_ICALL_PPPPP_V;
		} else if (INTERP_TYPE_AS_PTR (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params [0]) &&
					INTERP_TYPE_AS_PTR (sig->params [1]) &&
					INTERP_TYPE_AS_PTR (sig->params [2]) &&
					INTERP_TYPE_AS_PTR (sig->params [3]) &&
					INTERP_TYPE_AS_PTR (sig->params [4]))
				op = MINT_ICALL_PPPPP_P;
		}
		break;
	case 6:
		if (MONO_TYPE_IS_VOID (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params [0]) &&
					INTERP_TYPE_AS_PTR (sig->params [1]) &&
					INTERP_TYPE_AS_PTR (sig->params [2]) &&
					INTERP_TYPE_AS_PTR (sig->params [3]) &&
					INTERP_TYPE_AS_PTR (sig->params [4]) &&
					INTERP_TYPE_AS_PTR (sig->params [5]))
				op = MINT_ICALL_PPPPPP_V;
		} else if (INTERP_TYPE_AS_PTR (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params [0]) &&
					INTERP_TYPE_AS_PTR (sig->params [1]) &&
					INTERP_TYPE_AS_PTR (sig->params [2]) &&
					INTERP_TYPE_AS_PTR (sig->params [3]) &&
					INTERP_TYPE_AS_PTR (sig->params [4]) &&
					INTERP_TYPE_AS_PTR (sig->params [5]))
				op = MINT_ICALL_PPPPPP_P;
		}
		break;
	}
	return op;
}

/* Same as mono jit */
#define INLINE_LENGTH_LIMIT 20
#define INLINE_DEPTH_LIMIT 10

gboolean
TransformData::interp_method_check_inlining (MonoMethod *method, MonoMethodSignature *csignature)
{
	MonoMethodHeaderSummary header;

	if (method->flags & METHOD_ATTRIBUTE_REQSECOBJ)
		/* Used to mark methods containing StackCrawlMark locals */
		return FALSE;

	if (csignature->call_convention == MONO_CALL_VARARG)
		return FALSE;

	if (!mono_method_get_header_summary (method, &header))
		return FALSE;

	/*runtime, icall and pinvoke are checked by summary call*/
	if ((method->iflags & METHOD_IMPL_ATTRIBUTE_NOINLINING) ||
	    (method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED) ||
	    (mono_class_is_marshalbyref (method->klass)) ||
	    header.has_clauses)
		return FALSE;

	if (inline_depth > INLINE_DEPTH_LIMIT)
		return FALSE;

	if (header.code_size >= INLINE_LENGTH_LIMIT && !(method->iflags & METHOD_IMPL_ATTRIBUTE_AGGRESSIVE_INLINING))
		return FALSE;

	if (mono_class_needs_cctor_run (method->klass, NULL)) {
		MonoVTable *vtable;
		ERROR_DECL (error);
		if (!m_class_get_runtime_info (method->klass))
			/* No vtable created yet */
			return FALSE;
		vtable = mono_class_vtable_checked (rtm->domain, method->klass, error);
		if (!is_ok (error)) {
			mono_interp_error_cleanup (error);
			return FALSE;
		}
		if (!vtable->initialized)
			return FALSE;
	}

	/* We currently access at runtime the wrapper data */
	if (method->wrapper_type != MONO_WRAPPER_NONE)
		return FALSE;

	/* Our usage of `emit_store_value_as_local ()` for nint, nuint and nfloat
	 * is kinda hacky, and doesn't work with the inliner */
	if (mono_class_get_magic_index (method->klass) >= 0)
		return FALSE;

	if (prof_coverage)
		return FALSE;

	if (std::find (dont_inline.begin (), dont_inline.end (), method)
	    != dont_inline.end ())
		return FALSE;

	/*
	 * An inlined body never gets a transform of its own, so this is the last
	 * point its IL can be put through the verifier. Declining the inline is
	 * all that is needed: the call stays a real one, and the callee is
	 * verified when it is transformed.
	 */
	{
		ERROR_DECL (verify_error);

		if (!mono_llvm_jit_verify_method (method, verify_error)) {
			mono_interp_error_cleanup (verify_error);
			return FALSE;
		}
	}

	return TRUE;
}

gboolean
TransformData::interp_inline_method (MonoMethod *target_method, MonoMethodHeader *header, MonoError *error)
{
	const unsigned char *prev_ip, *prev_il_code, *prev_in_start;
	int *prev_in_offsets;
	gboolean ret;
	unsigned int prev_max_stack_height, prev_locals_size;
	size_t prev_n_data_items;
	int i; 
	int prev_sp_offset;
	MonoGenericContext *generic_context = NULL;
	StackInfo *prev_param_area;
	InterpBasicBlock **prev_offset_to_bb;
	InterpBasicBlock *prev_cbb, *prev_entry_bb;
	MonoMethod *prev_inlined_method;
	MonoMethodSignature *csignature = mono_method_signature_internal (target_method);
	int nargs = csignature->param_count + !!csignature->hasthis;
	InterpInst *prev_last_ins;

	if (csignature->is_inflated)
		generic_context = mono_method_get_context (target_method);
	else {
		MonoGenericContainer *generic_container = mono_method_get_generic_container (target_method);
		if (generic_container)
			generic_context = &generic_container->context;
	}

	prev_ip = ip;
	prev_il_code = il_code;
	prev_in_start = in_start;
	prev_sp_offset = sp - stack;
	prev_inlined_method = inlined_method;
	prev_last_ins = last_ins;
	prev_offset_to_bb = offset_to_bb;
	prev_cbb = cbb;
	prev_entry_bb = entry_bb;
	inlined_method = target_method;

	prev_max_stack_height = max_stack_height;
	prev_locals_size = locals.size ();

	prev_n_data_items = data_items.size ();
	prev_in_offsets = in_offsets;
	in_offsets = (int*)g_malloc0((header->code_size + 1) * sizeof(int));


	/* Inlining pops the arguments, restore the stack */
	prev_param_area = (StackInfo*)g_malloc (nargs * sizeof (StackInfo));
	memcpy (prev_param_area, &sp [-nargs], nargs * sizeof (StackInfo));

	int const prev_code_size = code_size;
	code_size = header->code_size;

	if (verbose_level)
		g_print ("Inline start method %s.%s\n", m_class_get_name (target_method->klass), target_method->name);

	inline_depth++;
	ret = generate_code (target_method, header, generic_context, error);
	inline_depth--;

	if (!ret) {
		if (!is_ok (error))
			mono_interp_error_cleanup (error);

		if (verbose_level)
			g_print ("Inline aborted method %s.%s\n", m_class_get_name (target_method->klass), target_method->name);
		max_stack_height = prev_max_stack_height;
		locals.resize (prev_locals_size);

		/* Remove any newly added items */
		for (size_t item = prev_n_data_items; item < data_items.size (); item++)
			data_hash.erase (data_items[item]);
		data_items.resize (prev_n_data_items);
		sp = stack + prev_sp_offset;
		memcpy (&sp [-nargs], prev_param_area, nargs * sizeof (StackInfo));
		last_ins = prev_last_ins;
		cbb = prev_cbb;
		if (last_ins)
			last_ins->next = NULL;
		UnlockedIncrement (&mono_interp_stats.inline_failures);
	} else {
		if (verbose_level)
			g_print ("Inline end method %s.%s\n", m_class_get_name (target_method->klass), target_method->name);
		UnlockedIncrement (&mono_interp_stats.inlined_methods);

		interp_link_bblocks (prev_cbb, entry_bb);
		prev_cbb->next_bb = entry_bb;
	}

	ip = prev_ip;
	in_start = prev_in_start;
	il_code = prev_il_code;
	inlined_method = prev_inlined_method;
	offset_to_bb = prev_offset_to_bb;
	code_size = prev_code_size;
	entry_bb = prev_entry_bb;

	g_free (in_offsets);
	in_offsets = prev_in_offsets;

	g_free (prev_param_area);
	return ret;
}

void
TransformData::interp_constrained_box (MonoDomain *domain, MonoClass *constrained_class, MonoMethodSignature *csignature, MonoError *error)
{
	MintType mt = mint_type (m_class_get_byval_arg (constrained_class));
	StackInfo *sp = this->sp - 1 - csignature->param_count;
	if (mono_class_is_nullable (constrained_class)) {
		g_assert (mt == MintType::VT);
		interp_add_ins (MINT_BOX_NULLABLE_PTR);
		last_ins->data [0] = get_data_item_index (constrained_class);
	} else {
		MonoVTable *vtable = mono_class_vtable_checked (domain, constrained_class, error);
		return_if_nok (error);

		interp_add_ins (MINT_BOX_PTR);
		last_ins->data [0] = get_data_item_index (vtable);
	}
	interp_ins_set_sreg (last_ins, sp->local);
	set_simple_type_and_local (sp, StackType::O);
	interp_ins_set_dreg (last_ins, sp->local);
}

static MonoMethod*
interp_get_method (MonoMethod *method, guint32 token, MonoImage *image, MonoGenericContext *generic_context, MonoError *error)
{
	if (method->wrapper_type == MONO_WRAPPER_NONE)
		return mono_get_method_checked (image, token, NULL, generic_context, error);
	else
		return (MonoMethod *)mono_method_get_wrapper_data (method, token);
}

/*
 * Whether an argument may travel through a call that hands the caller's frame away.
 *
 * Both ends of a tail call promise the callee touches nothing of the frame it replaces,
 * so anything that could point into it has to stay behind. The signature covers the
 * declared cases; the stack type covers a managed pointer travelling under a signature
 * that no longer says so, which is unverifiable but cheap to spot here.
 *
 * A native int is not refused. It is how the frame-address checks in the tailcall
 * corpus travel, the value is compared rather than dereferenced, and the compiled
 * engine passes them through as well.
 */
static gboolean
interp_tail_call_arg_is_safe (MonoType *param, StackType stack_type)
{
	if (param->byref)
		return FALSE;

	if (param->type == MONO_TYPE_PTR || param->type == MONO_TYPE_FNPTR)
		return FALSE;

	return stack_type != StackType::MP;
}

/*
 * Whether the two returns can be folded into one: the tail callee writes its result
 * where the method being replaced would have written its own, which is a slot the
 * caller's caller sized from the signature it called.
 */
static gboolean
interp_tail_call_returns_match (MonoType *caller_ret, MonoType *callee_ret)
{
	MintType mt;

	caller_ret = mini_type_get_underlying_type (caller_ret);
	callee_ret = mini_type_get_underlying_type (callee_ret);

	/* mint_type () has no answer for void, and a method returning nothing can only
	 * hand its return over to another that returns nothing. */
	if (caller_ret->type == MONO_TYPE_VOID || callee_ret->type == MONO_TYPE_VOID)
		return caller_ret->type == callee_ret->type;

	mt = mint_type (caller_ret);
	if (mt != mint_type (callee_ret))
		return FALSE;

	/* MINT_RET_VT copies the callee's own size into that slot, so it has to be the
	 * size the slot was made for. Everything else travels as a whole stackval, which
	 * makes the narrow integer types interchangeable. */
	if (mt == MintType::VT)
		return mono_class_value_size (mono_class_from_mono_type_internal (caller_ret), NULL) ==
			mono_class_value_size (mono_class_from_mono_type_internal (callee_ret), NULL);

	return TRUE;
}

/*
 * Whether the tail. prefix at the current call site can be honoured by handing this
 * frame to the callee rather than making an ordinary call.
 *
 * Every shape the LLVM back end honours in should_tail_call () has to be honoured here
 * too, since a method runs in whichever engine happens to own it and under tier 0 in
 * both at different times - a guarantee only one of them keeps is not a guarantee. The
 * reverse does not hold: the compiler declines several shapes for reasons about
 * prototypes and argument registers that mean nothing to an engine whose calling
 * convention is a byte image laid out from the call site's signature, and this refuses
 * them only where the interpreter has a reason of its own.
 *
 * td->sp still holds the arguments, so this has to run before they are popped.
 */
static const char*
interp_tail_call_refusal (TransformData *td, MonoMethod *method, MonoMethod *target_method,
			  MonoMethodSignature *csignature, gboolean calli, gboolean is_virtual, int op)
{
	MonoMethodHeader *header = td->header;
	int in_offset = td->ip - td->il_code;
	int nargs = csignature->param_count + !!csignature->hasthis;
	int i;

	/* An intrinsified call is emitted inline and has no frame to hand over. */
	if (op != -1)
		return "the call is intrinsified";

	/*
	 * A dispatched call is fine: the target is resolved to an InterpMethod at the site,
	 * before the frame changes hands, and every override reads its arguments from the
	 * layout the site's signature already produced. An indirect one is not done here -
	 * nothing needs it yet, and its target may turn out to be a p/invoke.
	 */
	if (calli)
		return "the target is indirect";
	if (target_method == NULL)
		return "the target is unknown";
	/* A marshalbyref target goes through a remoting check that wants a frame. */
	if (is_virtual && mono_class_is_marshalbyref (target_method->klass))
		return "the target may be remote";

	/* A frame that is gone has nowhere to stop at, and the method-exit sequence point
	 * that a step-out lands on is one the folded return never reaches. */
	if (td->gen_sdb_seq_points)
		return "the debugger is watching";

	/* Under either of these CEE_RET emits MINT_PROF_EXIT, which does the return
	 * itself. A tail call would jump past it. */
	if (td->rtm->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE)
		return "the profiler wants the method exit";
	if (mono_jit_trace_calls != NULL && mono_trace_eval (method))
		return "the method is traced";

	/* A vararg caller's cookie buffer is allocated in the frame being handed away and
	 * read for the whole of the callee's execution; a vararg callee's own signature is
	 * not the one the site pushed. */
	if (td->rtm->vararg)
		return "the caller is vararg";
	if (csignature->call_convention == MONO_CALL_VARARG)
		return "the callee is vararg";

	/* Neither is entered by running IL: the transition saves state that a jump
	 * straight into the callee would skip. */
	if (target_method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)
		return "the callee is a p/invoke";
	if (target_method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
		return "the callee is an internal call";

	/*
	 * III.2.4 forbids the prefix inside a protected block, and a reused frame would
	 * leave the clause ranges describing a method that is no longer running.
	 * td->clause_indexes only covers handler bodies, so the try and filter ranges have
	 * to be looked up here.
	 */
	for (i = 0; i < header->num_clauses; i++) {
		MonoExceptionClause *c = header->clauses + i;

		if (MONO_OFFSET_IN_CLAUSE (c, in_offset) || MONO_OFFSET_IN_HANDLER (c, in_offset))
			return "the site is inside a protected block";
		if (c->flags == MONO_EXCEPTION_CLAUSE_FILTER &&
		    in_offset >= (int)c->data.filter_offset && in_offset < (int)c->handler_offset)
			return "the site is inside a filter";
	}

	/* The prefix promises a ret follows at once, which is what lets the two returns
	 * fold into one, and the arguments must be all the evaluation stack holds. */
	if (in_offset + 5 >= td->code_size || *(td->ip + 5) != CEE_RET)
		return "no ret follows the call";
	if (td->sp - td->stack != nargs)
		return "the evaluation stack outlives the call";

	if (!interp_tail_call_returns_match (mono_method_signature_internal (method)->ret, csignature->ret))
		return "the two returns are shaped differently";

	/* A value type's this is a pointer to the value, and the value is usually a local
	 * of the frame being handed away. */
	if (csignature->hasthis && m_class_is_valuetype (target_method->klass))
		return "this is a value type";

	for (i = 0; i < csignature->param_count; i++) {
		if (!interp_tail_call_arg_is_safe (csignature->params [i], td->sp [i - csignature->param_count].type))
			return "an argument may point into the frame";
	}

	return NULL;
}

/* Return FALSE if error, including inline failure */
gboolean
TransformData::interp_transform_call (MonoMethod *method, MonoMethod *target_method, MonoDomain *domain, MonoGenericContext *generic_context, MonoClass *constrained_class, gboolean readonly, MonoError *error, gboolean check_visibility, gboolean save_last_error, gboolean tailcall)
{
	MonoImage *image = m_class_get_image (method->klass);
	MonoMethodSignature *csignature;
	int is_virtual = *ip == CEE_CALLVIRT;
	int calli_extra_arg = *ip == CEE_MONO_CALLI_EXTRA_ARG;
	int calli = *ip == CEE_CALLI || calli_extra_arg;
	int i;
	guint32 res_size = 0;
	int op = -1;
	int native = 0;
	int need_null_check = is_virtual;
	int fp_sreg = -1, first_sreg = -1, dreg = -1;
	gboolean is_delegate_invoke = FALSE;
	gboolean emit_tailcall = FALSE;

	guint32 token = read32 (ip + 1);

	if (target_method == NULL) {
		if (calli) {
			CHECK_STACK (1);
			if (method->wrapper_type != MONO_WRAPPER_NONE)
				csignature = (MonoMethodSignature *)mono_method_get_wrapper_data (method, token);
			else {
				csignature = mono_metadata_parse_signature_checked (image, token, error);
				return_val_if_nok (error, FALSE);
			}

			if (generic_context) {
				csignature = mono_inflate_generic_signature (csignature, generic_context, error);
				return_val_if_nok (error, FALSE);
			}

			/*
			 * The compiled interp entry wrapper is passed to runtime_invoke instead of
			 * the InterpMethod pointer. FIXME
			 */
			native = csignature->pinvoke || method->wrapper_type == MONO_WRAPPER_RUNTIME_INVOKE;

			target_method = NULL;
		} else {
			target_method = interp_get_method (method, token, image, generic_context, error);
			return_val_if_nok (error, FALSE);
			csignature = mono_method_signature_internal (target_method);

			if (generic_context) {
				csignature = mono_inflate_generic_signature (csignature, generic_context, error);
				return_val_if_nok (error, FALSE);
				target_method = mono_class_inflate_generic_method_checked (target_method, generic_context, error);
				return_val_if_nok (error, FALSE);
			}
		}
	} else {
		csignature = mono_method_signature_internal (target_method);
	}

	if (check_visibility && target_method && !mono_method_can_access_method (method, target_method))
		interp_generate_mae_throw (method, target_method);

	if (target_method && target_method->string_ctor) {
		/* Create the real signature */
		MonoMethodSignature *ctor_sig = mono_metadata_signature_dup_mempool (arena.pool (), csignature);
		ctor_sig->ret = m_class_get_byval_arg (mono_defaults.string_class);

		csignature = ctor_sig;
	}

	/* Ahead of the intrinsics, which read the arguments where they lie. A
	 * calli's function pointer is above them, so it is not the last argument. */
	StackInfo *args = this->sp - csignature->param_count - (calli ? 1 : 0);

	for (i = 0; i < csignature->param_count; i++) {
		if (args [i].type == StackType::R4 || args [i].type == StackType::R8)
			coerce_fp (args + i, fp_stack_type (csignature->params [i]));
	}

	/* Intrinsics */
	if (target_method && interp_handle_intrinsics (target_method, constrained_class, csignature, readonly, &op))
		return TRUE;

	if (constrained_class) {
		if (m_class_is_enumtype (constrained_class) && !strcmp (target_method->name, "GetHashCode")) {
			/* Use the corresponding method from the base type to avoid boxing */
			MonoType *base_type = mono_class_enum_basetype_internal (constrained_class);
			g_assert (base_type);
			constrained_class = mono_class_from_mono_type_internal (base_type);
			target_method = mono_class_get_method_from_name_checked (constrained_class, target_method->name, 0, 0, error);
			mono_error_assert_ok (error);
			g_assert (target_method);
		}
	}

	if (constrained_class) {
		mono_class_setup_vtable (constrained_class);
		if (mono_class_has_failure (constrained_class)) {
			mono_error_set_for_class_failure (error, constrained_class);
			return FALSE;
		}
#if DEBUG_INTERP
		g_print ("CONSTRAINED.CALLVIRT: %s::%s.  %s (%p) ->\n", target_method->klass->name, target_method->name, mono_signature_full_name (target_method->signature), target_method);
#endif
		target_method = mono_get_method_constrained_with_method (image, target_method, constrained_class, generic_context, error);
#if DEBUG_INTERP
		g_print ("                    : %s::%s.  %s (%p)\n", target_method->klass->name, target_method->name, mono_signature_full_name (target_method->signature), target_method);
#endif
		/* Intrinsics: Try again, it could be that `mono_get_method_constrained_with_method` resolves to a method that we can substitute */
		if (target_method && interp_handle_intrinsics (target_method, constrained_class, csignature, readonly, &op))
			return TRUE;

		return_val_if_nok (error, FALSE);
		mono_class_setup_vtable (target_method->klass);

		// Follow the rules for constrained calls from ECMA spec
		if (!m_class_is_valuetype (constrained_class)) {
			StackInfo *sp = this->sp - 1 - csignature->param_count;
			/* managed pointer on the stack, we need to deref that puppy */
			interp_add_ins (MINT_LDIND_I);
			interp_ins_set_sreg (last_ins, sp->local);
			set_simple_type_and_local (sp, StackType::I);
			interp_ins_set_dreg (last_ins, sp->local);
		} else if (target_method->klass != constrained_class) {
			/*
			 * The type parameter is instantiated as a valuetype,
			 * but that type doesn't override the method we're
			 * calling, so we need to box `this'.
			 */
			StackType this_type = (this->sp - csignature->param_count - 1)->type;
			g_assert (this_type == StackType::I || this_type == StackType::MP);
			interp_constrained_box (domain, constrained_class, csignature, error);
			return_val_if_nok (error, FALSE);
		} else {
			is_virtual = FALSE;
		}
	}

	if (target_method)
		mono_class_init_internal (target_method->klass);

	if (!is_virtual && target_method && (target_method->flags & METHOD_ATTRIBUTE_ABSTRACT)) {
		if (!mono_class_is_interface (method->klass))
			interp_generate_bie_throw ();
		else
			is_virtual = TRUE;
	}

	if (is_virtual && target_method && (!(target_method->flags & METHOD_ATTRIBUTE_VIRTUAL) ||
		(MONO_METHOD_IS_FINAL (target_method) &&
		 target_method->wrapper_type != MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK)) &&
		!(mono_class_is_marshalbyref (target_method->klass))) {
		/* Not really virtual, just needs a null check */
		is_virtual = FALSE;
		need_null_check = TRUE;
	}

	CHECK_STACK (csignature->param_count + csignature->hasthis);
	if (tailcall) {
		const char *refusal = interp_tail_call_refusal (this, method, target_method, csignature, calli, is_virtual, op);

		/* A declined tail call is an ordinary call and return, so nothing about it
		 * shows up in what the program does. This is the only way to tell one from a
		 * site that took the jump. */
		if (refusal && verbose_level)
			g_print ("Decline tail call at IL_%04x: %s\n", (int)(ip - il_code), refusal);

		emit_tailcall = refusal == NULL;
	}

	if (emit_tailcall) {
		(void)mono_class_vtable_checked (domain, target_method->klass, error);
		return_val_if_nok (error, FALSE);

		/*
		 * A method calling itself needs no call at all: write the arguments back over
		 * the incoming ones and branch to the top. That skips the argument move and
		 * the frame bookkeeping a general tail call pays for. It has to be the method
		 * itself and not merely the one named at the site, so a dispatched call - which
		 * may land on an override - keeps its resolution.
		 */
		if (method == target_method && !is_virtual) {
			if (inlined_method)
				return FALSE;

			if (verbose_level)
				g_print ("Optimize tail call of %s.%s\n", m_class_get_name (target_method->klass), target_method->name);

			for (i = csignature->param_count - 1 + !!csignature->hasthis; i >= 0; --i)
				store_arg (i);

			/*
			 * The next invocation owns none of this one's localloc memory, and the
			 * branch never reaches a ret to release it. Whether the method locallocs
			 * at all is not settled until the whole body has been read, so this is
			 * emitted unconditionally; it costs a load and a predictable branch.
			 */
			interp_add_ins (MINT_LOCALLOC_UNWIND);

			interp_add_ins (MINT_BR_S);
			// We are branching to the beginning of the method
			last_ins->info.target_bb = entry_bb;
			int in_offset = ip - il_code;
			if (interp_ip_in_cbb (in_offset + 5))
				++ip; /* gobble the CEE_RET if it isn't branched to */
			ip += 5;
			return TRUE;
		}
	}

	target_method = interp_transform_internal_calls (method, target_method, csignature, is_virtual);

	if (csignature->call_convention == MONO_CALL_VARARG)
		csignature = mono_method_get_signature_checked (target_method, image, token, generic_context, error);

	/* A null check tests the receiver, so a signature with no this has nothing to
	 * test. A callvirt that names a static method arrives here: the block above
	 * degrades it to a plain call, and the slot under the arguments then belongs
	 * to the caller instead of to this call. */
	if (need_null_check && csignature->hasthis) {
		StackInfo *sp = this->sp - 1 - csignature->param_count;
		interp_add_ins (MINT_CKNULL);
		interp_ins_set_sreg (last_ins, sp->local);
		set_simple_type_and_local (sp, sp->type);
		interp_ins_set_dreg (last_ins, sp->local);
	}

	g_assert (csignature->call_convention != MONO_CALL_FASTCALL);
	if ((mono_interp_opt & INTERP_OPT_INLINE) && op == -1 && !is_virtual && target_method && interp_method_check_inlining (target_method, csignature)) {
		MonoMethodHeader *mheader = interp_method_get_header (target_method, error);
		return_val_if_nok (error, FALSE);

		if (interp_inline_method (target_method, mheader, error)) {
			ip += 5;
			return TRUE;
		}
	}

	/* Don't inline methods that do calls */
	if (op == -1 && inlined_method)
		return FALSE;

	/* We need to convert delegate invoke to a indirect call on the interp_invoke_impl field */
	if (target_method && m_class_get_parent (target_method->klass) == mono_defaults.multicastdelegate_class) {
		const char *name = target_method->name;
		if (*name == 'I' && (strcmp (name, "Invoke") == 0))
			is_delegate_invoke = TRUE;
	}

	/* Pop the function pointer */
	if (calli) {
		--this->sp;
		fp_sreg = this->sp [0].local;
		locals [fp_sreg].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
	}

	/* The callee reads each argument at the width its parameter is declared at. */
	for (i = 0; i < csignature->param_count; i++)
		widen_i4_to_i8 (this->sp - csignature->param_count + i, csignature->params [i]);

	guint32 tos_offset = get_tos_offset ();
	this->sp -= csignature->param_count + !!csignature->hasthis;
	guint32 params_stack_size = tos_offset - get_tos_offset ();

	if (op == -1 || num_dregs (op) == MINT_CALL_ARGS) {
		// We must not optimize out these locals, storing to them is part of the interp call convention
		// unless we already intrinsified this call
		for (int i = 0; i < (csignature->param_count + !!csignature->hasthis); i++)
			locals [this->sp [i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
	}

	// We overwrite it with the return local, save it for future use
	if (csignature->param_count || csignature->hasthis)
		first_sreg = this->sp [0].local;

	/* need to handle typedbyref ... */
	if (csignature->ret->type != MONO_TYPE_VOID) {
		MintType mt = mint_type(csignature->ret);
		MonoClass *klass = mono_class_from_mono_type_internal (csignature->ret);

		if (mt == MintType::VT) {
			if (csignature->pinvoke && method->wrapper_type != MONO_WRAPPER_NONE)
				res_size = mono_class_native_size (klass, NULL);
			else
				res_size = mono_class_value_size (klass, NULL);
			push_type_vt (klass, res_size);
			res_size = ALIGN_TO (res_size, MINT_VT_ALIGNMENT);
			if (mono_class_has_failure (klass)) {
				mono_error_set_for_class_failure (error, klass);
				return FALSE;
			}
		} else {
			push_type (stack_type_of (mt), klass);
			res_size = MINT_STACK_SLOT_SIZE;
		}
		dreg = this->sp [-1].local;
		if (op == -1 || num_dregs (op) == MINT_CALL_ARGS) {
			// This dreg needs to be at the same offset as the call args
			locals [dreg].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
		}
	} else {
		// Create a new dummy local to serve as the dreg of the call
		// This dreg is only used to resolve the call args offset
		push_simple_type (StackType::I4);
		this->sp--;
		dreg = this->sp [0].local;
	}

	if (op >= 0) {
		interp_add_ins (op);

		int has_dreg = num_dregs (op);
		int sregs_count = num_sregs (op);
		if (has_dreg)
			interp_ins_set_dreg (last_ins, dreg);
		if (sregs_count > 0) {
			if (sregs_count == 1)
				interp_ins_set_sreg (last_ins, first_sreg);
			else if (sregs_count == 2)
				interp_ins_set_sregs2 (last_ins, first_sreg, this->sp [!has_dreg].local);
			else if (sregs_count == 3)
				interp_ins_set_sregs3 (last_ins, first_sreg, this->sp [!has_dreg].local, this->sp [!has_dreg + 1].local);
			else
				g_error ("Unsupported opcode");
		}
		
		if (op == MINT_LDLEN) {
#ifdef MONO_BIG_ARRAYS
			SET_SIMPLE_TYPE (this->sp - 1, StackType::I8);
#else
			SET_SIMPLE_TYPE (this->sp - 1, StackType::I4);
#endif
		}

#ifndef ENABLE_NETCORE
		if (op == MINT_CALLRUN) {
			interp_ins_set_dreg (last_ins, dreg);
			last_ins->data [0] = get_data_item_index (target_method);
			last_ins->data [1] = get_data_item_index (mono_method_signature_internal (target_method));
		}
#endif
	} else if (!calli && !is_delegate_invoke && !is_virtual && !emit_tailcall && mono_interp_jit_call_supported (target_method, csignature)) {
		interp_add_ins (MINT_JIT_CALL);
		interp_ins_set_dreg (last_ins, dreg);
		last_ins->data [0] = get_data_item_index ((void *)mono_interp_get_imethod (domain, target_method, error));
		mono_error_assert_ok (error);
	} else {
		if (is_delegate_invoke) {
			interp_add_ins (MINT_CALL_DELEGATE);
			interp_ins_set_dreg (last_ins, dreg);
			last_ins->data [0] = params_stack_size;
			last_ins->data [1] = get_data_item_index ((void *)csignature);
		} else if (calli) {
#ifndef MONO_ARCH_HAS_NO_PROPER_MONOCTX
			/* Try using fast icall path for simple signatures */
			if (native && !method->dynamic)
				op = interp_icall_op_for_sig (csignature);
#endif
			// FIXME calli receives both the args offset and sometimes another arg for the frame pointer,
			// therefore some args are in the param area, while the fp is not. We should differentiate for
			// this, probably once we will have an explicit param area where we copy arguments.
			if (op != -1) {
				interp_add_ins (MINT_CALLI_NAT_FAST);
				interp_ins_set_dreg (last_ins, dreg);
				last_ins->data [0] = get_data_item_index ((void *)csignature);
				last_ins->data [1] = op;
				last_ins->data [2] = save_last_error;
			} else if (native && method->dynamic && csignature->pinvoke) {
				interp_add_ins (MINT_CALLI_NAT_DYNAMIC);
				interp_ins_set_dreg (last_ins, dreg);
				interp_ins_set_sreg (last_ins, fp_sreg);
				last_ins->data [0] = get_data_item_index ((void *)csignature);
			} else if (native) {
				interp_add_ins (MINT_CALLI_NAT);
#ifdef TARGET_X86
				/* Windows not tested/supported yet */
				g_assertf (csignature->call_convention == MONO_CALL_DEFAULT || csignature->call_convention == MONO_CALL_C, "Interpreter supports only cdecl pinvoke on x86");
#endif

				InterpMethod *imethod = NULL;
				/*
				 * We can have pinvoke calls outside M2N wrappers, in xdomain calls, where we can't easily get the called imethod.
				 * Those calls will be slower since we will not cache the arg offsets on the imethod, and have to compute them
				 * every time based on the signature.
				 */
				if (method->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE) {
					WrapperInfo *info = mono_marshal_get_wrapper_info (method);
					if (info) {
						MonoMethod *pinvoke_method = info->d.managed_to_native.method;
						imethod = mono_interp_get_imethod (domain, pinvoke_method, error);
						return_val_if_nok (error, FALSE);
					}
				}

				interp_ins_set_dreg (last_ins, dreg);
				interp_ins_set_sreg (last_ins, fp_sreg);
				last_ins->data [0] = get_data_item_index (csignature);
				last_ins->data [1] = get_data_item_index (imethod);
				last_ins->data [2] = save_last_error;
				/* Cache slot */
				last_ins->data [3] = get_data_item_index_nonshared (NULL);
			} else if (calli_extra_arg) {
				/*
				 * Only a delegate-invoke wrapper emits this, and only over what
				 * MINT_LD_DELEGATE_METHOD_PTR just pushed - the delegate's target
				 * as an InterpMethod, which needs no resolving.
				 */
				interp_add_ins (MINT_CALLI_IMETHOD);
				interp_ins_set_dreg (last_ins, dreg);
				interp_ins_set_sreg (last_ins, fp_sreg);
				last_ins->data [0] = get_data_item_index ((void *)csignature);
			} else {
				interp_add_ins (MINT_CALLI);
				interp_ins_set_dreg (last_ins, dreg);
				interp_ins_set_sreg (last_ins, fp_sreg);
				last_ins->data [0] = get_data_item_index ((void *)csignature);
				/* Cache slot for the entry point this site last resolved */
				last_ins->data [1] = get_data_item_index_nonshared (NULL);
			}
		} else {
			InterpMethod *imethod = mono_interp_get_imethod (domain, target_method, error);
			return_val_if_nok (error, FALSE);

			if (csignature->call_convention == MONO_CALL_VARARG) {
				interp_add_ins (MINT_CALL_VARARG);
				last_ins->data [1] = get_data_item_index ((void *)csignature);
				last_ins->data [2] = params_stack_size;
			} else if (is_virtual && !mono_class_is_marshalbyref (target_method->klass)) {
				interp_add_ins (emit_tailcall ? MINT_TAILCALLVIRT_FAST : MINT_CALLVIRT_FAST);
				if (mono_class_is_interface (target_method->klass))
					last_ins->data [1] = -2 * MONO_IMT_SIZE + mono_method_get_imt_slot (target_method);
				else
					last_ins->data [1] = mono_method_get_vtable_slot (target_method);
				/* How much of this frame the callee is handed as its arguments. */
				if (emit_tailcall)
					last_ins->data [2] = params_stack_size;
			} else if (is_virtual) {
				interp_add_ins (MINT_CALLVIRT);
			} else if (emit_tailcall) {
				interp_add_ins (MINT_TAILCALL);
				last_ins->data [1] = params_stack_size;
			} else {
				interp_add_ins (MINT_CALL);
			}
			interp_ins_set_dreg (last_ins, dreg);
			last_ins->data [0] = get_data_item_index ((void *)imethod);
		}
	}
	ip += 5;

	return TRUE;
}

static MonoClassField *
interp_field_from_token (MonoMethod *method, guint32 token, MonoClass **klass, MonoGenericContext *generic_context, MonoError *error)
{
	MonoClassField *field = NULL;
	if (method->wrapper_type != MONO_WRAPPER_NONE) {
		field = (MonoClassField *) mono_method_get_wrapper_data (method, token);
		*klass = field->parent;

		mono_class_setup_fields (field->parent);
	} else {
		field = mono_field_from_token_checked (m_class_get_image (method->klass), token, klass, generic_context, error);
		return_val_if_nok (error, NULL);
	}

	if (!method->skip_visibility && !mono_method_can_access_field (method, field)) {
		char *method_fname = mono_method_full_name (method, TRUE);
		char *field_fname = mono_field_full_name (field);
		mono_error_set_generic_error (error, "System", "FieldAccessException", "Field `%s' is inaccessible from method `%s'\n", field_fname, method_fname);
		g_free (method_fname);
		g_free (field_fname);
		return NULL;
	}

	return field;
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

static guint8*
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

/*
 * interp_save_line_numbers:
 *
 *   Keep the bytecode offset -> IL offset map the transform built, so a stack
 * trace can report an IL offset for a frame of this method.
 */
void
TransformData::interp_save_line_numbers (InterpMethod *rtm, const std::vector<MonoDebugLineNumberEntry> &line_numbers)
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
	rtm->line_numbers = (guint8*) mono_mem_manager_alloc0 (mem_manager, rtm->line_numbers_size);
	memcpy (rtm->line_numbers, buf, rtm->line_numbers_size);
	g_free (buf);

	mono_atomic_fetch_add_i32 (&mono_interp_stats.line_numbers_size, (gint32) rtm->line_numbers_size);
}

void
TransformData::interp_save_debug_info (InterpMethod *rtm, MonoMethodHeader *header, const std::vector<MonoDebugLineNumberEntry> &line_numbers)
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
	dinfo->code_start = (guint8*)rtm->code;
	dinfo->code_size = new_code_end - new_code;
	dinfo->epilogue_begin = 0;
	dinfo->has_var_info = TRUE;
	dinfo->num_line_numbers = (int) line_numbers.size ();
	dinfo->line_numbers = g_new0 (MonoDebugLineNumberEntry, dinfo->num_line_numbers);

	for (i = 0; i < dinfo->num_params; i++) {
		MonoDebugVarInfo *var = &dinfo->params [i];
		var->type = rtm->param_types [i];
	}
	for (i = 0; i < dinfo->num_locals; i++) {
		MonoDebugVarInfo *var = &dinfo->locals [i];
		var->type = mono_metadata_type_dup (NULL, header->locals [i]);
	}

	std::copy (line_numbers.begin (), line_numbers.end (), dinfo->line_numbers);
	mono_debug_add_method (rtm->method, dinfo, rtm->domain);

	mono_debug_free_method_jit_info (dinfo);
}

/* Same as the code in seq-points.c */
static void
insert_pred_seq_point (SeqPoint *last_sp, SeqPoint *sp, GSList **next)
{
	GSList *l;
	int src_index = last_sp->next_offset;
	int dst_index = sp->next_offset;

	/* bb->in_bb might contain duplicates */
	for (l = next [src_index]; l; l = l->next)
		if (GPOINTER_TO_UINT (l->data) == dst_index)
			break;
	if (!l)
		next [src_index] = g_slist_append (next [src_index], GUINT_TO_POINTER (dst_index));
}

void
TransformData::recursively_make_pred_seq_points (InterpBasicBlock *bb)
{
	SeqPoint ** const MONO_SEQ_SEEN_LOOP = (SeqPoint**)GINT_TO_POINTER(-1);

	GArray *predecessors = g_array_new (FALSE, TRUE, sizeof (gpointer));
	GHashTable *seen = g_hash_table_new_full (g_direct_hash, NULL, NULL, NULL);

	// Insert/remove sentinel into the memoize table to detect loops containing bb
	bb->pred_seq_points = MONO_SEQ_SEEN_LOOP;

	for (int i = 0; i < bb->in_count; ++i) {
		InterpBasicBlock *in_bb = bb->in_bb [i];

		// This bb has the last seq point, append it and continue
		if (in_bb->last_seq_point != NULL) {
			predecessors = g_array_append_val (predecessors, in_bb->last_seq_point);
			continue;
		}

		// We've looped or handled this before, exit early.
		// No last sequence points to find.
		if (in_bb->pred_seq_points == MONO_SEQ_SEEN_LOOP)
			continue;

		// Take sequence points from incoming basic blocks

		if (in_bb == entry_bb)
			continue;

		if (in_bb->pred_seq_points == NULL)
			recursively_make_pred_seq_points (in_bb);

		// Union sequence points with incoming bb's
		for (int i=0; i < in_bb->num_pred_seq_points; i++) {
			if (!g_hash_table_lookup (seen, in_bb->pred_seq_points [i])) {
				g_array_append_val (predecessors, in_bb->pred_seq_points [i]);
				g_hash_table_insert (seen, in_bb->pred_seq_points [i], (gpointer)&MONO_SEQ_SEEN_LOOP);
			}
		}
		// predecessors = g_array_append_vals (predecessors, in_bb->pred_seq_points, in_bb->num_pred_seq_points);
	}

	g_hash_table_destroy (seen);

	if (predecessors->len != 0) {
		bb->pred_seq_points = arena.create_array<SeqPoint *> (predecessors->len);
		bb->num_pred_seq_points = predecessors->len;

		for (int newer = 0; newer < bb->num_pred_seq_points; newer++) {
			bb->pred_seq_points [newer] = (SeqPoint*)g_array_index (predecessors, gpointer, newer);
		}
	}

	g_array_free (predecessors, TRUE);
}

void
TransformData::collect_pred_seq_points (InterpBasicBlock *bb, SeqPoint *seqp, GSList **next)
{
	// Doesn't have a last sequence point, must find from incoming basic blocks
	if (bb->pred_seq_points == NULL && bb != entry_bb)
		recursively_make_pred_seq_points (bb);

	for (int i = 0; i < bb->num_pred_seq_points; i++)
		insert_pred_seq_point (bb->pred_seq_points [i], seqp, next);

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
	 * following it, this is needed to implement 'step over' in the debugger agent.
	 */
	for (size_t i = 0; i < seq_points.size (); ++i)
		/* Store the seq point index here temporarily */
		seq_points[i]->next_offset = (int) i;

	next = arena.create_array<GSList *> (seq_points.size ());
	for (InterpBasicBlock *bb : basic_blocks) {
		GSList *bb_seq_points = g_slist_reverse (bb->seq_points);
		SeqPoint *last = NULL;
		for (GSList *l = bb_seq_points; l; l = l->next) {
			SeqPoint *sp = (SeqPoint*)l->data;

			if (sp->il_offset == METHOD_ENTRY_IL_OFFSET || sp->il_offset == METHOD_EXIT_IL_OFFSET)
				/* Used to implement method entry/exit events */
				continue;

			if (last != NULL) {
				/* Link with the previous seq point in the same bb */
				next [last->next_offset] = g_slist_append_mempool (arena.pool (), next [last->next_offset], GINT_TO_POINTER (sp->next_offset));
			} else {
				/* Link with the last bb in the previous bblocks */
				collect_pred_seq_points (bb, sp, next);
			}
			last = sp;
		}
	}

	/* Serialize the seq points into a byte array */
	array = g_byte_array_new ();
	SeqPoint zero_seq_point = {0};
	SeqPoint* last_seq_point = &zero_seq_point;
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

#define BARRIER_IF_VOLATILE(kind) \
	do { \
		if (volatile_) { \
			interp_emit_memory_barrier (kind); \
			volatile_ = FALSE; \
		} \
	} while (0)

#define INLINE_FAILURE \
	do { \
		if (inlining) \
			goto exit; \
	} while (0)

void
TransformData::interp_method_compute_offsets (InterpMethod *imethod, MonoMethodSignature *sig, MonoMethodHeader *header, MonoError *error)
{
	int i, offset, size, align;
	int num_args = sig->hasthis + sig->param_count;
	int num_il_locals = header->num_locals;
	int num_locals = num_args + num_il_locals;

	imethod->local_offsets = (guint32*)g_malloc (num_il_locals * sizeof(guint32));
	locals.resize (num_locals);
	offset = 0;

	g_assert (MINT_STACK_SLOT_SIZE == MINT_VT_ALIGNMENT);

	/*
	 * We will load arguments as if they are locals. Unlike normal locals, every argument
	 * is stored in a stackval sized slot and valuetypes have special semantics since we
	 * receive a pointer to the valuetype data rather than the data itself.
	 */
	for (i = 0; i < num_args; i++) {
		MonoType *type;
		if (sig->hasthis && i == 0)
			type = m_class_get_byval_arg (method->klass);
		else
			type = mono_method_signature_internal (method)->params [i - sig->hasthis];
		MintType mt = mint_type (type);
		locals [i].type = type;
		locals [i].offset = offset;
		locals [i].flags = 0;
		locals [i].indirects = 0;
		locals [i].mt = mt;
		if (mt == MintType::VT && (!sig->hasthis || i != 0)) {
			size = mono_type_size (type, &align);
			locals [i].size = size;
			offset += ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		} else {
			locals [i].size = MINT_STACK_SLOT_SIZE; // not really
			offset += MINT_STACK_SLOT_SIZE;
		}
	}

	il_locals_offset = offset;
	for (i = 0; i < num_il_locals; ++i) {
		int index = num_args + i;
		size = mono_type_size (header->locals [i], &align);
		if (header->locals [i]->type == MONO_TYPE_VALUETYPE) {
			if (mono_class_has_failure (header->locals [i]->data.klass)) {
				mono_error_set_for_class_failure (error, header->locals [i]->data.klass);
				return;
			}
		}
		offset += align - 1;
		offset &= ~(align - 1);
		imethod->local_offsets [i] = offset;
		locals [index].type = header->locals [i];
		locals [index].offset = offset;
		locals [index].flags = 0;
		locals [index].indirects = 0;
		locals [index].mt = mint_type (header->locals [i]);
		if (locals [index].mt == MintType::VT)
			locals [index].size = size;
		else
			locals [index].size = MINT_STACK_SLOT_SIZE; // not really
		// Every local takes a MINT_STACK_SLOT_SIZE so IL locals have same behavior as execution locals
		offset += ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
	}
	offset = ALIGN_TO (offset, MINT_VT_ALIGNMENT);
	il_locals_size = offset - il_locals_offset;

	imethod->clause_data_offsets = (guint32*)g_malloc (header->num_clauses * sizeof (guint32));
	for (i = 0; i < header->num_clauses; i++) {
		imethod->clause_data_offsets [i] = offset;
		offset += sizeof (MonoObject*);
	}
	offset = ALIGN_TO (offset, MINT_VT_ALIGNMENT);

	//g_assert (offset < G_MAXUINT16);
	total_locals_size = offset;
}


static gboolean
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

void
TransformData::interp_handle_isinst (MonoClass *klass, gboolean isinst_instr)
{
	/* Follow the logic from jit's handle_isinst */
	if (!mono_class_has_variant_generic_params (klass)) {
		if (mono_class_is_interface (klass))
			interp_add_ins (isinst_instr ? MINT_ISINST_INTERFACE : MINT_CASTCLASS_INTERFACE);
		else if (!mono_class_is_marshalbyref (klass) && m_class_get_rank (klass) == 0 && !mono_class_is_nullable (klass))
			interp_add_ins (isinst_instr ? MINT_ISINST_COMMON : MINT_CASTCLASS_COMMON);
		else
			interp_add_ins (isinst_instr ? MINT_ISINST : MINT_CASTCLASS);
	} else {
		interp_add_ins (isinst_instr ? MINT_ISINST : MINT_CASTCLASS);
	}
	sp--;
	interp_ins_set_sreg (last_ins, sp [0].local);
	if (isinst_instr)
		push_type (sp [0].type, sp [0].klass);
	else
		push_type (StackType::O, klass);
	interp_ins_set_dreg (last_ins, sp [-1].local);
	last_ins->data [0] = get_data_item_index (klass);

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
		interp_ins_set_dreg (last_ins, sp [-1].local);
		WRITE32_INS(last_ins, 0, &offset);
		last_ins->data [2] = get_data_item_index (vtable);
	} else {
		interp_add_ins (MINT_LDSFLDA);
		interp_ins_set_dreg (last_ins, sp [-1].local);
		last_ins->data [0] = get_data_item_index (vtable);
		last_ins->data [1] = get_data_item_index ((char*)mono_vtable_get_static_field_data (vtable) + field->offset);
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
			val = *(gint8*)field_addr;
			break;
		case MintType::U1:
			val = *(guint8*)field_addr;
			break;
		case MintType::I2:
			val = *(gint16*)field_addr;
			break;
		case MintType::U2:
			val = *(guint16*)field_addr;
			break;
		default:
			val = *(gint32*)field_addr;
		}
		interp_get_ldc_i4_from_const (NULL, val, sp [-1].local);
	} else if (mt == MintType::I8) {
		gint64 val = *(gint64*)field_addr;
		interp_add_ins (MINT_LDC_I8);
		interp_ins_set_dreg (last_ins, sp [-1].local);
		WRITE64_INS (last_ins, 0, &val);
	} else if (mt == MintType::R4) {
		float val = *(float*)field_addr;
		interp_add_ins (MINT_LDC_R4);
		interp_ins_set_dreg (last_ins, sp [-1].local);
		WRITE32_INS (last_ins, 0, &val);
	} else if (mt == MintType::R8) {
		double val = *(double*)field_addr;
		interp_add_ins (MINT_LDC_R8);
		interp_ins_set_dreg (last_ins, sp [-1].local);
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
TransformData::interp_emit_sfld_access (MonoClassField *field, MonoClass *field_class, MintType mt, gboolean is_load, MonoError *error)
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
		 * The vtable rides along in the last operand. The offset says where the
		 * storage is and nothing about which class owns it, so the handler has
		 * nothing else to run the class initializer from -- and the first touch
		 * of one of these from outside its own class is exactly when it has to
		 * run.
		 */
		int vtable_index = get_data_item_index (vtable);

		// Offset is SpecialStaticOffset
		if ((offset & 0x80000000) == 0 && mt != MintType::VT) {
			// This field is thread static
			if (is_load) {
				interp_add_ins (op_for_mint_type (MINT_LDTSFLD_I1, mt));
				WRITE32_INS(last_ins, 0, &offset);
				push_type (stack_type_of (mt), field_class);
				interp_ins_set_dreg (last_ins, sp [-1].local);
			} else {
				interp_add_ins (op_for_mint_type (MINT_STTSFLD_I1, mt));
				WRITE32_INS(last_ins, 0, &offset);
				sp--;
				interp_ins_set_sreg (last_ins, sp [0].local);
			}
			last_ins->data [2] = vtable_index;
		} else {
			if (mt == MintType::VT) {
				int size = mono_class_value_size (field_class, NULL);
				g_assert (size < G_MAXUINT16);
				if (is_load) {
					interp_add_ins (MINT_LDSSFLD_VT);
					push_type_vt (field_class, size);
					interp_ins_set_dreg (last_ins, sp [-1].local);
				} else {
					interp_add_ins (MINT_STSSFLD_VT);
					sp--;
					interp_ins_set_sreg (last_ins, sp [0].local);
				}
				WRITE32_INS(last_ins, 0, &offset);
				last_ins->data [2] = size;
				last_ins->data [3] = vtable_index;
			} else {
				if (is_load) {
					interp_add_ins (MINT_LDSSFLD);
					push_type (stack_type_of (mt), field_class);
					interp_ins_set_dreg (last_ins, sp [-1].local);
				} else {
					interp_add_ins (MINT_STSSFLD);
					sp--;
					interp_ins_set_sreg (last_ins, sp [0].local);
				}
				last_ins->data [0] = get_data_item_index (field);
				WRITE32_INS(last_ins, 1, &offset);
				last_ins->data [3] = vtable_index;
			}
		}
	} else {
		gpointer field_addr = (char*)mono_vtable_get_static_field_data (vtable) + field->offset;
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
			interp_ins_set_dreg (last_ins, sp [-1].local);
		} else {
			interp_add_ins ((mt == MintType::VT) ? MINT_STSFLD_VT : op_for_mint_type (MINT_STSFLD_I1, mt));
			sp--;
			interp_ins_set_sreg (last_ins, sp [0].local);
		}

		last_ins->data [0] = get_data_item_index (vtable);
		last_ins->data [1] = get_data_item_index ((char*)field_addr);
		if (mt == MintType::VT)
			last_ins->data [2] = size;

	}
}

void
TransformData::initialize_clause_bblocks ()
{
	MonoMethodHeader *header = this->header;
	int i;

	for (i = 0; i < header->code_size; i++)
		clause_indexes [i] = -1;

	for (i = 0; i < header->num_clauses; i++) {
		MonoExceptionClause *c = header->clauses + i;
		InterpBasicBlock *bb;

		for (int j = c->handler_offset; j < c->handler_offset + c->handler_len; j++) {
			if (clause_indexes [j] == -1)
				clause_indexes [j] = i;
		}

		bb = offset_to_bb [c->try_offset];
		g_assert (bb);
		bb->eh_block = TRUE;

		/* We never inline methods with clauses, so we can hard code stack heights */
		bb = offset_to_bb [c->handler_offset];
		g_assert (bb);
		bb->eh_block = TRUE;

		if (c->flags == MONO_EXCEPTION_CLAUSE_FINALLY) {
			bb->stack_height = 0;
		} else {
			bb->stack_height = 1;
			bb->stack_state = arena.create<StackInfo> ();
			bb->stack_state [0].type = StackType::O;
			bb->stack_state [0].klass = NULL; /*FIX*/
			bb->stack_state [0].size = MINT_STACK_SLOT_SIZE;
			bb->stack_state [0].offset = 0;
			bb->stack_state [0].local = create_interp_stack_local (StackType::O, NULL, MINT_STACK_SLOT_SIZE, 0);
		}

		if (c->flags == MONO_EXCEPTION_CLAUSE_FILTER) {
			bb = offset_to_bb [c->data.filter_offset];
			g_assert (bb);
			bb->eh_block = TRUE;
			bb->stack_height = 1;
			bb->stack_state = arena.create<StackInfo> ();
			bb->stack_state [0].type = StackType::O;
			bb->stack_state [0].klass = NULL; /*FIX*/
			bb->stack_state [0].size = MINT_STACK_SLOT_SIZE;
			bb->stack_state [0].offset = 0;
			bb->stack_state [0].local = create_interp_stack_local (StackType::O, NULL, MINT_STACK_SLOT_SIZE, 0);
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
	interp_ins_set_sreg (last_ins, sp [0].local);
	push_simple_type (type);
	interp_ins_set_dreg (last_ins, sp [-1].local);

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
	interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);

	++ip;
}

void
TransformData::handle_ldelem (int op, StackType type)
{
	CHECK_STACK (2);
	narrow_index (sp - 1);
	interp_add_ins (op);
	sp -= 2;
	interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
	push_simple_type (type);
	interp_ins_set_dreg (last_ins, sp [-1].local);
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
	interp_ins_set_sregs3 (last_ins, sp [0].local, sp [1].local, sp [2].local);
	++ip;
}

/// Keeps a method off the inline candidates for as long as the scope lives,
/// which is what stops a method being inlined into itself.
class DontInlineScope {
public:
	DontInlineScope (TransformData *td, MonoMethod *method) : td_ (td)
	{
		td_->dont_inline.push_back (method);
	}

	~DontInlineScope () { td_->dont_inline.pop_back (); }

	DontInlineScope (const DontInlineScope &) = delete;
	DontInlineScope &operator= (const DontInlineScope &) = delete;

private:
	TransformData *td_;
};

gboolean
TransformData::generate_code (MonoMethod *method, MonoMethodHeader *header, MonoGenericContext *generic_context, MonoError *error)
{
	int target;
	int offset, i, i32;
	MintType mt;
	guint32 token;
	int in_offset;
	const unsigned char *end;
	MonoSimpleBasicBlock *bb = NULL, *original_bb = NULL;
	gboolean sym_seq_points = FALSE;
	MonoBitSet *seq_point_locs = NULL;
	gboolean readonly = FALSE;
	gboolean volatile_ = FALSE;
	gboolean tailcall = FALSE;
	MonoClass *constrained_class = NULL;
	MonoClass *klass;
	MonoClassField *field;
	MonoImage *image = m_class_get_image (method->klass);
	InterpMethod *rtm = this->rtm;
	MonoDomain *domain = rtm->domain;
	MonoMethodSignature *signature = mono_method_signature_internal (method);
	int num_args = signature->hasthis + signature->param_count;
	int arglist_local = -1;
	gboolean ret = TRUE;
	gboolean emitted_funccall_seq_point = FALSE;
	guint32 *arg_locals = NULL;
	guint32 *local_locals = NULL;
	InterpInst *last_seq_point = NULL;
	gboolean save_last_error = FALSE;
	gboolean link_bblocks = TRUE;
	gboolean inlining = this->method != method;
	InterpBasicBlock *exit_bb = NULL;

	DontInlineScope dont_inline (this, method);

	original_bb = bb = mono_basic_block_split (method, error, header);
	goto_if_nok (error, exit);
	g_assert (bb);

	il_code = header->code;
	in_start = ip = header->code;
	end = ip + header->code_size;

	cbb = entry_bb = arena.create<InterpBasicBlock> ();
	cbb->index = bb_count++;
	cbb->native_offset = -1;
	cbb->stack_height = sp - stack;

	if (inlining) {
		exit_bb = arena.create<InterpBasicBlock> ();
		exit_bb->index = bb_count++;
		exit_bb->native_offset = -1;
		exit_bb->stack_height = -1;
	}

	get_basic_blocks (header, gen_sdb_seq_points);

	if (!inlining)
		initialize_clause_bblocks ();

	if (gen_sdb_seq_points && !inlining) {
		MonoDebugMethodInfo *minfo;

		minfo = mono_debug_lookup_method (method);

		if (minfo) {
			MonoSymSeqPoint *sps;
			int i, n_il_offsets;

			mono_debug_get_seq_points (minfo, NULL, NULL, NULL, &sps, &n_il_offsets);
			// FIXME: Free
			seq_point_locs = mono_bitset_mem_new (arena.alloc0 (mono_bitset_alloc_size (header->code_size, 0), alignof (gsize)), header->code_size, 0);
			sym_seq_points = TRUE;

			for (i = 0; i < n_il_offsets; ++i) {
				if (sps [i].il_offset < header->code_size)
					mono_bitset_set_fast (seq_point_locs, sps [i].il_offset);
			}
			g_free (sps);

			MonoDebugMethodAsyncInfo* asyncMethod = mono_debug_lookup_method_async_debug_info (method);
			if (asyncMethod) {
				for (i = 0; asyncMethod != NULL && i < asyncMethod->num_awaits; i++) {
					mono_bitset_set_fast (seq_point_locs, asyncMethod->resume_offsets [i]);
					mono_bitset_set_fast (seq_point_locs, asyncMethod->yield_offsets [i]);
				}
				mono_debug_free_method_async_debug_info (asyncMethod);
			}
		} else if (!method->wrapper_type && !method->dynamic && mono_debug_image_has_debug_info (m_class_get_image (method->klass))) {
			/* Methods without line number info like auto-generated property accessors */
			seq_point_locs = mono_bitset_new (header->code_size, 0);
			sym_seq_points = TRUE;
		}
	}

	if (sym_seq_points) {
		last_seq_point = interp_add_ins (MINT_SDB_SEQ_POINT);
		last_seq_point->flags |= INTERP_INST_FLAG_SEQ_POINT_METHOD_ENTRY;
	}

	if (mono_debugger_method_has_breakpoint (method)) {
		interp_add_ins (MINT_BREAKPOINT);
	}

	if (!inlining) {
		if (verbose_level) {
			char *tmp = mono_disasm_code (NULL, method, ip, end);
			char *name = mono_method_full_name (method, TRUE);
			g_print ("Method %s, original code:\n", name);
			g_print ("%s\n", tmp);
			g_free (tmp);
			g_free (name);
		}

		if (rtm->vararg) {
			// vararg calls are identical to normal calls on the call site. However, the
			// first instruction in a vararg method needs to copy the variable arguments
			// into a special region so they can be accessed by MINT_ARGLIST. This region
			// is localloc'ed so we have compile time static offsets for all locals/stack.
			arglist_local = create_interp_local (m_class_get_byval_arg (mono_defaults.int_class));
			interp_add_ins (MINT_INIT_ARGLIST);
			interp_ins_set_dreg (last_ins, arglist_local);
			// This is the offset where the variable args are on stack. After this instruction
			// which copies them to localloc'ed memory, this space will be overwritten by normal
			// locals
			last_ins->data [0] = il_locals_offset;
			has_localloc = TRUE;
		}

		/*
		 * We initialize the locals regardless of the presence of the init_locals
		 * flag. Locals holding references need to be zeroed so we don't risk
		 * crashing the GC if they end up being stored in an object.
		 *
		 * FIXME
		 * Track values of locals over multiple basic blocks. This would enable
		 * us to kill the MINT_INITLOCALS instruction if all locals are initialized
		 * before use. We also don't need this instruction if the init locals flag
		 * is not set and there are no locals holding references.
		 */
		if (header->num_locals) {
			interp_add_ins (MINT_INITLOCALS);
			last_ins->data [0] = il_locals_offset;
			last_ins->data [1] = il_locals_size;
		}

		guint16 enter_profiling = 0;
		if (mono_jit_trace_calls != NULL && mono_trace_eval (method))
			enter_profiling |= TRACING_FLAG;
		if (rtm->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_ENTER)
			enter_profiling |= PROFILING_FLAG;
		if (enter_profiling) {
			interp_add_ins (MINT_PROF_ENTER);
			last_ins->data [0] = enter_profiling;
		}

		/*
		 * If safepoints are required by default, always check for polling,
		 * without emitting new instructions. This optimizes method entry in
		 * the common scenario, which is coop.
		 */
#if !defined(ENABLE_HYBRID_SUSPEND) && !defined(ENABLE_COOP_SUSPEND)
		/* safepoint is required on method entry */
		if (mono_threads_are_safepoints_enabled ())
			interp_add_ins (MINT_SAFEPOINT);
#endif
	} else {
		int local;
		arg_locals = (guint32*) g_malloc ((!!signature->hasthis + signature->param_count) * sizeof (guint32));
		/* Allocate locals to store inlined method args from stack */
		for (i = signature->param_count - 1; i >= 0; i--) {
			local = create_interp_local (signature->params [i]);
			arg_locals [i + !!signature->hasthis] = local;
			store_local (local);
		}

		if (signature->hasthis) {
			/*
			 * If this is value type, it is passed by address and not by value.
			 * Valuetype this local gets integer type MintType::I.
			 */
			MonoType *type;
			if (m_class_is_valuetype (method->klass))
				type = mono_get_int_type ();
			else
				type = mono_get_object_type ();
			local = create_interp_local (type);
			arg_locals [0] = local;
			store_local (local);
		}

		local_locals = (guint32*) g_malloc (header->num_locals * sizeof (guint32));
		/* Allocate locals to store inlined method args from stack */
		for (i = 0; i < header->num_locals; i++)
			local_locals [i] = create_interp_local (header->locals [i]);
	}

	while (ip < end) {
		g_assert (sp >= stack);
		in_offset = ip - header->code;
		if (!inlining)
			current_il_offset = in_offset;

		InterpBasicBlock *new_bb = offset_to_bb [in_offset];
		if (new_bb != NULL && cbb != new_bb) {
			/* We are starting a new basic block. Change cbb and link them together */
			if (link_bblocks) {
				/*
				 * By default we link cbb with the new starting bblock, unless the previous
				 * instruction is an unconditional branch (BR, LEAVE, ENDFINALLY)
				 */
				interp_link_bblocks (cbb, new_bb);
				fixup_newbb_stack_locals (new_bb);
			}
			cbb->next_bb = new_bb;
			cbb = new_bb;

			if (new_bb->stack_height >= 0) {
				if (new_bb->stack_height > 0)
					memcpy (stack, new_bb->stack_state, new_bb->stack_height * sizeof(stack [0]));
				sp = stack + new_bb->stack_height;
			} else if (link_bblocks) {
				/* This bblock is not branched to. Initialize its stack state */
				init_bb_stack_state (new_bb);
			}
			link_bblocks = TRUE;
			if (!inlining) {
				int index = clause_indexes [in_offset];
				if (index != -1) {
					MonoExceptionClause *clause = &header->clauses [index];
					if ((clause->flags == MONO_EXCEPTION_CLAUSE_FINALLY ||
						clause->flags == MONO_EXCEPTION_CLAUSE_FAULT) &&
							in_offset == clause->handler_offset)
						interp_add_ins (MINT_START_ABORT_PROT);
				}
			}

		}
		offset_to_bb [in_offset] = cbb;
		in_start = ip;

		if (in_offset == bb->end)
			bb = bb->next;

		if (bb->dead) {
			int op_size = mono_opcode_size (ip, end);
			g_assert (op_size > 0); /* The BB formation pass must catch all bad ops */

			if (verbose_level > 1)
				g_print ("SKIPPING DEAD OP at %x\n", in_offset);
			link_bblocks = FALSE;
			ip += op_size;
			continue;
		}

		if (verbose_level > 1) {
			g_print ("IL_%04lx %-10s, sp %ld, %s %-12s\n",
				ip - il_code,
				mono_opcode_name (*ip), sp - stack,
				sp > stack ? stack_type_string [(int) sp [-1].type] : "  ",
				(sp > stack && (sp [-1].type == StackType::O || sp [-1].type == StackType::VT)) ? (sp [-1].klass == NULL ? "?" : m_class_get_name (sp [-1].klass)) : "");
		}

		if (sym_seq_points && mono_bitset_test_fast (seq_point_locs, ip - header->code)) {
			if (in_offset == 0 || (header->num_clauses && !cbb->last_ins))
				interp_add_ins (MINT_SDB_INTR_LOC);
			last_seq_point = interp_add_ins (MINT_SDB_SEQ_POINT);
		}

		if (prof_coverage) {
			guint32 cil_offset = ip - header->code;
			gpointer counter = &coverage_info->data [cil_offset].count;
			coverage_info->data [cil_offset].cil_code = (unsigned char*)ip;

			interp_add_ins (MINT_PROF_COVERAGE_STORE);
			WRITE64_INS (last_ins, 0, &counter);
		}

		switch (*ip) {
		case CEE_NOP: 
			/* lose it */
			emitted_funccall_seq_point = FALSE;
			++ip;
			break;
		case CEE_BREAK:
			interp_add_ins (MINT_BREAK);
			++ip;
			break;
		case CEE_LDARG_0:
		case CEE_LDARG_1:
		case CEE_LDARG_2:
		case CEE_LDARG_3: {
			int arg_n = *ip - CEE_LDARG_0;
			if (!inlining)
				load_arg (arg_n);
			else
				load_local (arg_locals [arg_n]);
			++ip;
			break;
		}
		case CEE_LDLOC_0:
		case CEE_LDLOC_1:
		case CEE_LDLOC_2:
		case CEE_LDLOC_3: {
			int loc_n = *ip - CEE_LDLOC_0;
			if (!inlining)
				load_local (num_args + loc_n);
			else
				load_local (local_locals [loc_n]);
			++ip;
			break;
		}
		case CEE_STLOC_0:
		case CEE_STLOC_1:
		case CEE_STLOC_2:
		case CEE_STLOC_3: {
			int loc_n = *ip - CEE_STLOC_0;
			if (!inlining)
				store_local (num_args + loc_n);
			else
				store_local (local_locals [loc_n]);
			++ip;
			break;
		}
		case CEE_LDARG_S: {
			int arg_n = ((guint8 *)ip)[1];
			if (!inlining)
				load_arg (arg_n);
			else
				load_local (arg_locals [arg_n]);
			ip += 2;
			break;
		}
		case CEE_LDARGA_S: {
			/* NOTE: n includes this */
			int n = ((guint8 *) ip) [1];

			if (!inlining) {
				interp_add_ins (MINT_LDLOCA_S);
				interp_ins_set_sreg (last_ins, n);
				locals [n].indirects++;
			} else {
				int loc_n = arg_locals [n];
				interp_add_ins (MINT_LDLOCA_S);
				interp_ins_set_sreg (last_ins, loc_n);
				locals [loc_n].indirects++;
			}
			push_simple_type (StackType::MP);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 2;
			break;
		}
		case CEE_STARG_S: {
			int arg_n = ((guint8 *)ip)[1];
			if (!inlining)
				store_arg (arg_n);
			else
				store_local (arg_locals [arg_n]);
			ip += 2;
			break;
		}
		case CEE_LDLOC_S: {
			int loc_n = ((guint8 *)ip)[1];
			if (!inlining)
				load_local (num_args + loc_n);
			else
				load_local (local_locals [loc_n]);
			ip += 2;
			break;
		}
		case CEE_LDLOCA_S: {
			int loc_n = ((guint8 *)ip)[1];
			interp_add_ins (MINT_LDLOCA_S);
			if (!inlining)
				loc_n += num_args;
			else
				loc_n = local_locals [loc_n];
			interp_ins_set_sreg (last_ins, loc_n);
			locals [loc_n].indirects++;
			push_simple_type (StackType::MP);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 2;
			break;
		}
		case CEE_STLOC_S: {
			int loc_n = ((guint8 *)ip)[1];
			if (!inlining)
				store_local (num_args + loc_n);
			else
				store_local (local_locals [loc_n]);
			ip += 2;
			break;
		}
		case CEE_LDNULL: 
			interp_add_ins (MINT_LDNULL);
			push_type (StackType::O, NULL);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			++ip;
			break;
		case CEE_LDC_I4_M1:
			interp_add_ins (MINT_LDC_I4_M1);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			++ip;
			break;
		case CEE_LDC_I4_0:
			if (in_offset + 2 < code_size && interp_ip_in_cbb (in_offset + 1) && ip [1] == 0xfe && ip [2] == CEE_CEQ &&
				sp > stack && sp [-1].type == StackType::I4) {
				interp_add_ins (MINT_CEQ0_I4);
				sp--;
				interp_ins_set_sreg (last_ins, sp [0].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				ip += 3;
			} else {
				interp_add_ins (MINT_LDC_I4_0);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				++ip;
			}
			break;
		case CEE_LDC_I4_1:
			if (in_offset + 1 < code_size && interp_ip_in_cbb (in_offset + 1) &&
				(ip [1] == CEE_ADD || ip [1] == CEE_SUB) && sp [-1].type == StackType::I4) {
				interp_add_ins (ip [1] == CEE_ADD ? MINT_ADD1_I4 : MINT_SUB1_I4);
				sp--;
				interp_ins_set_sreg (last_ins, sp [0].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				ip += 2;
			} else {
				interp_add_ins (MINT_LDC_I4_1);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				++ip;
			}
			break;
		case CEE_LDC_I4_2:
		case CEE_LDC_I4_3:
		case CEE_LDC_I4_4:
		case CEE_LDC_I4_5:
		case CEE_LDC_I4_6:
		case CEE_LDC_I4_7:
		case CEE_LDC_I4_8:
			interp_add_ins ((*ip - CEE_LDC_I4_0) + MINT_LDC_I4_0);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			++ip;
			break;
		case CEE_LDC_I4_S: 
			interp_add_ins (MINT_LDC_I4_S);
			last_ins->data [0] = ((gint8 *) ip) [1];
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 2;
			break;
		case CEE_LDC_I4:
			i32 = read32 (ip + 1);
			interp_add_ins (MINT_LDC_I4);
			WRITE32_INS (last_ins, 0, &i32);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 5;
			break;
		case CEE_LDC_I8: {
			gint64 val = read64 (ip + 1);
			interp_add_ins (MINT_LDC_I8);
			WRITE64_INS (last_ins, 0, &val);
			push_simple_type (StackType::I8);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 9;
			break;
		}
		case CEE_LDC_R4: {
			float val;
			readr4 (ip + 1, &val);
			interp_add_ins (MINT_LDC_R4);
			WRITE32_INS (last_ins, 0, &val);
			push_simple_type (StackType::R4);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 5;
			break;
		}
		case CEE_LDC_R8: {
			double val;
			readr8 (ip + 1, &val);
			interp_add_ins (MINT_LDC_R8);
			WRITE64_INS (last_ins, 0, &val);
			push_simple_type (StackType::R8);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			ip += 9;
			break;
		}
		case CEE_DUP: {
			StackType type = sp [-1].type;
			MonoClass *klass = sp [-1].klass;
			MintType mt = locals [sp [-1].local].mt;
			if (mt == MintType::VT) {
				gint32 size = mono_class_value_size (klass, NULL);
				g_assert (size < G_MAXUINT16);

				interp_add_ins (MINT_MOV_VT);
				interp_ins_set_sreg (last_ins, sp [-1].local);
				push_type_vt (klass, size);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				last_ins->data [0] = size;
			} else  {
				interp_add_ins (get_mov_for_type (mt, FALSE));
				interp_ins_set_sreg (last_ins, sp [-1].local);
				push_type (type, klass);
				interp_ins_set_dreg (last_ins, sp [-1].local);
			}
			ip++;
			break;
		}
		case CEE_POP:
			CHECK_STACK (1);
			interp_add_ins (MINT_NOP);
			--sp;
			++ip;
			break;
		case CEE_JMP: {
			MonoMethod *m;
			INLINE_FAILURE;
			if (sp > stack)
				g_warning ("CEE_JMP: stack must be empty");
			token = read32 (ip + 1);
			m = mono_get_method_checked (image, token, NULL, generic_context, error);
			goto_if_nok (error, exit);
			interp_add_ins (MINT_JMP);
			last_ins->data [0] = get_data_item_index (mono_interp_get_imethod (domain, m, error));
			goto_if_nok (error, exit);
			ip += 5;
			break;
		}
		case CEE_CALLVIRT: /* Fall through */
		case CEE_CALLI:    /* Fall through */
		case CEE_CALL: {
			gboolean need_seq_point = FALSE;

			if (sym_seq_points && !mono_bitset_test_fast (seq_point_locs, ip + 5 - header->code))
				need_seq_point = TRUE;

			if (!interp_transform_call (method, NULL, domain, generic_context, constrained_class, readonly, error, TRUE, save_last_error, tailcall))
				goto exit;

			if (need_seq_point) {
				//check is is a nested call and remove the MONO_INST_NONEMPTY_STACK of the last breakpoint, only for non native methods
				if (!(method->flags & METHOD_IMPL_ATTRIBUTE_NATIVE)) {
					if (emitted_funccall_seq_point)	{
						if (last_seq_point)
							last_seq_point->flags |= INTERP_INST_FLAG_SEQ_POINT_NESTED_CALL;
					}
					else
						emitted_funccall_seq_point = TRUE;	
				}
				last_seq_point = interp_add_ins (MINT_SDB_SEQ_POINT);
				// This seq point is actually associated with the instruction following the call
				last_seq_point->il_offset = ip - header->code;
				last_seq_point->flags = INTERP_INST_FLAG_SEQ_POINT_NONEMPTY_STACK;
			}

			constrained_class = NULL;
			readonly = FALSE;
			save_last_error = FALSE;
			tailcall = FALSE;
			break;
		}
		case CEE_RET: {
			MonoType *ult = mini_type_get_underlying_type (signature->ret);
			int vt_size = 0;

			link_bblocks = FALSE;

			/* Before the inlined case leaves, since the value is this method's either way. */
			if (ult->type != MONO_TYPE_VOID) {
				CHECK_STACK (1);
				emit_convert (ult);
			}

			/* Return from inlined method, return value is on top of stack */
			if (inlining) {
				ip++;
				fixup_newbb_stack_locals (exit_bb);
				interp_add_ins (MINT_BR_S);
				last_ins->info.target_bb = exit_bb;
				init_bb_stack_state (exit_bb);
				interp_link_bblocks (cbb, exit_bb);
				break;
			}

			if (ult->type != MONO_TYPE_VOID) {
				--sp;
				if (mint_type (ult) == MintType::VT) {
					MonoClass *klass = mono_class_from_mono_type_internal (ult);
					vt_size = mono_class_value_size (klass, NULL);
				}
			}
			if (sp > stack) {
				mono_error_set_generic_error (error, "System", "InvalidProgramException", "");
				goto exit;
			}

			if (sym_seq_points) {
				last_seq_point = interp_add_ins (MINT_SDB_SEQ_POINT);
				last_ins->flags |= INTERP_INST_FLAG_SEQ_POINT_METHOD_EXIT;
			}

			guint16 exit_profiling = 0;
			if (mono_jit_trace_calls != NULL && mono_trace_eval (method))
				exit_profiling |= TRACING_FLAG;
			if (rtm->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE)
				exit_profiling |= PROFILING_FLAG;
			if (exit_profiling) {
				/* This does the return as well */
				interp_add_ins (MINT_PROF_EXIT);
				if (ult->type == MONO_TYPE_VOID) {
					vt_size = -1;
					interp_ins_set_sreg (last_ins, -1);
				} else {
					interp_ins_set_sreg (last_ins, sp [0].local);
				}
					
				last_ins->data [0] = exit_profiling;
				WRITE32_INS (last_ins, 1, &vt_size);
			} else {
				if (vt_size == 0) {
					if (ult->type == MONO_TYPE_VOID) {
						interp_add_ins (MINT_RET_VOID);
					} else {
						interp_add_ins (MINT_RET);
						interp_ins_set_sreg (last_ins, sp [0].local);
					}
				} else {
					interp_add_ins (MINT_RET_VT);
					g_assert (vt_size < G_MAXUINT16);
					interp_ins_set_sreg (last_ins, sp [0].local);
					last_ins->data [0] = vt_size;
				}
			}
			++ip;
			break;
		}
		case CEE_BR: {
			int offset = read32 (ip + 1);
			if (offset) {
				handle_branch (MINT_BR_S, MINT_BR, 5 + offset);
				link_bblocks = FALSE;
			}
			ip += 5;
			break;
		}
		case CEE_BR_S: {
			int offset = (gint8)ip [1];
			if (offset) {
				handle_branch (MINT_BR_S, MINT_BR, 2 + (gint8)ip [1]);
				link_bblocks = FALSE;
			}
			ip += 2;
			break;
		}
		case CEE_BRFALSE:
			one_arg_branch (MINT_BRFALSE_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BRFALSE_S:
			one_arg_branch (MINT_BRFALSE_I4, (gint8)ip [1], 2);
			ip += 2;
			break;
		case CEE_BRTRUE:
			one_arg_branch (MINT_BRTRUE_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BRTRUE_S:
			one_arg_branch (MINT_BRTRUE_I4, (gint8)ip [1], 2);
			ip += 2;
			break;
		case CEE_BEQ:
			two_arg_branch (MINT_BEQ_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BEQ_S:
			two_arg_branch (MINT_BEQ_I4, (gint8) ip [1], 2);
			ip += 2;
			break;
		case CEE_BGE:
			two_arg_branch (MINT_BGE_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BGE_S:
			two_arg_branch (MINT_BGE_I4, (gint8) ip [1], 2);
			ip += 2;
			break;
		case CEE_BGT:
			two_arg_branch (MINT_BGT_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BGT_S:
			two_arg_branch (MINT_BGT_I4, (gint8) ip [1], 2);
			ip += 2;
			break;
		case CEE_BLT:
			two_arg_branch (MINT_BLT_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BLT_S:
			two_arg_branch (MINT_BLT_I4, (gint8) ip [1], 2);
			ip += 2;
			break;
		case CEE_BLE:
			two_arg_branch (MINT_BLE_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BLE_S:
			two_arg_branch (MINT_BLE_I4, (gint8) ip [1], 2);
			ip += 2;
			break;
		case CEE_BNE_UN:
			two_arg_branch (MINT_BNE_UN_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BNE_UN_S:
			two_arg_branch (MINT_BNE_UN_I4, (gint8) ip [1], 2);
			ip += 2;
			break;
		case CEE_BGE_UN:
			two_arg_branch (MINT_BGE_UN_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BGE_UN_S:
			two_arg_branch (MINT_BGE_UN_I4, (gint8) ip [1], 2);
			ip += 2;
			break;
		case CEE_BGT_UN:
			two_arg_branch (MINT_BGT_UN_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BGT_UN_S:
			two_arg_branch (MINT_BGT_UN_I4, (gint8) ip [1], 2);
			ip += 2;
			break;
		case CEE_BLE_UN:
			two_arg_branch (MINT_BLE_UN_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BLE_UN_S:
			two_arg_branch (MINT_BLE_UN_I4, (gint8) ip [1], 2);
			ip += 2;
			break;
		case CEE_BLT_UN:
			two_arg_branch (MINT_BLT_UN_I4, read32 (ip + 1), 5);
			ip += 5;
			break;
		case CEE_BLT_UN_S:
			two_arg_branch (MINT_BLT_UN_I4, (gint8) ip [1], 2);
			ip += 2;
			break;
		case CEE_SWITCH: {
			guint32 n;
			const unsigned char *next_ip;
			++ip;
			n = read32 (ip);
			narrow_index (sp - 1);
			interp_add_ins_explicit (MINT_SWITCH, MINT_SWITCH_LEN (n));
			WRITE32_INS (last_ins, 0, &n);
			ip += 4;
			next_ip = ip + n * 4;
			--sp;
			interp_ins_set_sreg (last_ins, sp [0].local);
			InterpBasicBlock **target_bb_table = arena.create_array<InterpBasicBlock *> (n);
			for (i = 0; i < n; i++) {
				offset = read32 (ip);
				target = next_ip - il_code + offset;
				InterpBasicBlock *target_bb = offset_to_bb [target];
				g_assert (target_bb);
				if (offset < 0) {
#if DEBUG_INTERP
					if (stack_height > 0 && stack_height != target_bb->stack_height)
						g_warning ("SWITCH with back branch and non-empty stack");
#endif
				} else {
					init_bb_stack_state (target_bb);
				}
				target_bb_table [i] = target_bb;
				interp_link_bblocks (cbb, target_bb);
				ip += 4;
			}
			last_ins->info.target_bb_table = target_bb_table;
			break;
		}
		case CEE_LDIND_I1:
			handle_ldind (MINT_LDIND_I1_CHECK, StackType::I4, &volatile_);
			break;
		case CEE_LDIND_U1:
			handle_ldind (MINT_LDIND_U1_CHECK, StackType::I4, &volatile_);
			break;
		case CEE_LDIND_I2:
			handle_ldind (MINT_LDIND_I2_CHECK, StackType::I4, &volatile_);
			break;
		case CEE_LDIND_U2:
			handle_ldind (MINT_LDIND_U2_CHECK, StackType::I4, &volatile_);
			break;
		case CEE_LDIND_I4:
			handle_ldind (MINT_LDIND_I4_CHECK, StackType::I4, &volatile_);
			break;
		case CEE_LDIND_U4:
			handle_ldind (MINT_LDIND_U4_CHECK, StackType::I4, &volatile_);
			break;
		case CEE_LDIND_I8:
			handle_ldind (MINT_LDIND_I8_CHECK, StackType::I8, &volatile_);
			break;
		case CEE_LDIND_I:
			handle_ldind (MINT_LDIND_REF_CHECK, StackType::I, &volatile_);
			break;
		case CEE_LDIND_R4:
			handle_ldind (MINT_LDIND_R4_CHECK, StackType::R4, &volatile_);
			break;
		case CEE_LDIND_R8:
			handle_ldind (MINT_LDIND_R8_CHECK, StackType::R8, &volatile_);
			break;
		case CEE_LDIND_REF:
			handle_ldind (MINT_LDIND_REF_CHECK, StackType::O, &volatile_);
			break;
		case CEE_STIND_REF:
			handle_stind (MINT_STIND_REF, &volatile_);
			break;
		case CEE_STIND_I1:
			handle_stind (MINT_STIND_I1, &volatile_);
			break;
		case CEE_STIND_I2:
			handle_stind (MINT_STIND_I2, &volatile_);
			break;
		case CEE_STIND_I4:
			handle_stind (MINT_STIND_I4, &volatile_);
			break;
		case CEE_STIND_I:
			handle_stind (MINT_STIND_I, &volatile_);
			break;
		case CEE_STIND_I8:
			handle_stind (MINT_STIND_I8, &volatile_);
			break;
		case CEE_STIND_R4:
			handle_stind (MINT_STIND_R4, &volatile_);
			break;
		case CEE_STIND_R8:
			handle_stind (MINT_STIND_R8, &volatile_);
			break;
		case CEE_ADD:
			binary_arith_op (MINT_ADD_I4);
			++ip;
			break;
		case CEE_SUB:
			binary_arith_op (MINT_SUB_I4);
			++ip;
			break;
		case CEE_MUL:
			binary_arith_op (MINT_MUL_I4);
			++ip;
			break;
		case CEE_DIV:
			binary_arith_op (MINT_DIV_I4);
			++ip;
			break;
		case CEE_DIV_UN:
			binary_arith_op (MINT_DIV_UN_I4);
			++ip;
			break;
		case CEE_REM:
			binary_arith_op (MINT_REM_I4);
			++ip;
			break;
		case CEE_REM_UN:
			binary_arith_op (MINT_REM_UN_I4);
			++ip;
			break;
		case CEE_AND:
			binary_arith_op (MINT_AND_I4);
			++ip;
			break;
		case CEE_OR:
			binary_arith_op (MINT_OR_I4);
			++ip;
			break;
		case CEE_XOR:
			binary_arith_op (MINT_XOR_I4);
			++ip;
			break;
		case CEE_SHL:
			shift_op (MINT_SHL_I4);
			++ip;
			break;
		case CEE_SHR:
			shift_op (MINT_SHR_I4);
			++ip;
			break;
		case CEE_SHR_UN:
			shift_op (MINT_SHR_UN_I4);
			++ip;
			break;
		case CEE_NEG:
			unary_arith_op (MINT_NEG_I4);
			++ip;
			break;
		case CEE_NOT:
			unary_arith_op (MINT_NOT_I4);
			++ip;
			break;
		case CEE_CONV_U1:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U1_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U1_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U1_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U1_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_I1:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I1_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I1_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I1_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I1_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_U2:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U2_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U2_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U2_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U2_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_I2:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I2_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I2_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I2_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I2_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_U:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R8:
#if SIZEOF_VOID_P == 4
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_U4_R8);
#else
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_U8_R8);
#endif
				break;
			case StackType::I4:
#if SIZEOF_VOID_P == 8
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_I8_U4);
#endif
				break;
			case StackType::I8:
#if SIZEOF_VOID_P == 4
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_U4_I8);
#endif
				break;
			case StackType::MP:
			case StackType::O:
				SET_SIMPLE_TYPE(sp - 1, StackType::I);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_I: 
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R8:
#if SIZEOF_VOID_P == 8
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_I8_R8);
#else
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_I4_R8);
#endif
				break;
			case StackType::I4:
#if SIZEOF_VOID_P == 8
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_I8_I4);
#endif
				break;
			case StackType::O:
			case StackType::MP:
				SET_SIMPLE_TYPE(sp - 1, StackType::I);
				break;
			case StackType::I8:
#if SIZEOF_VOID_P == 4
				interp_add_conv (sp - 1, NULL, StackType::I, MINT_CONV_I4_I8);
#endif
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_U4:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U4_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U4_R8);
				break;
			case StackType::I4:
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U4_I8);
				break;
			case StackType::MP:
#if SIZEOF_VOID_P == 8
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_U4_I8);
#else
				SET_SIMPLE_TYPE (sp - 1, StackType::I4);
#endif
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_I4:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I4_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I4_R8);
				break;
			case StackType::I4:
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I4_I8);
				break;
			case StackType::MP:
#if SIZEOF_VOID_P == 8
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I4_I8);
#else
				SET_SIMPLE_TYPE (sp - 1, StackType::I4);
#endif
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_I8:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_R8);
				break;
			case StackType::I4: {
				if (interp_ins_is_ldc (last_ins) && last_ins == cbb->last_ins) {
					gint64 ct = interp_get_const_from_ldc_i4 (last_ins);
					interp_clear_ins (last_ins);

					interp_add_ins (MINT_LDC_I8);
					sp--;
					push_simple_type (StackType::I8);
					interp_ins_set_dreg (last_ins, sp [-1].local);
					WRITE64_INS (last_ins, 0, &ct);
				} else {
					interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_I4);
				}
				break;
			}
			case StackType::I8:
				break;
			case StackType::MP:
#if SIZEOF_VOID_P == 4
				interp_add_ins (MINT_CONV_I8_I4);
#else
				SET_SIMPLE_TYPE(sp - 1, StackType::I8);
#endif
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_R4:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::R4, MINT_CONV_R4_R8);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::R4, MINT_CONV_R4_I8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::R4, MINT_CONV_R4_I4);
				break;
			case StackType::R4:
				/* no-op */
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_R8:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R8_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R8_I8);
				break;
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R8_R4);
				break;
			case StackType::R8:
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_U8:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::I4:
				if (interp_ins_is_ldc (last_ins) && last_ins == cbb->last_ins) {
					gint64 ct = (guint32)interp_get_const_from_ldc_i4 (last_ins);
					interp_clear_ins (last_ins);

					interp_add_ins (MINT_LDC_I8);
					sp--;
					push_simple_type (StackType::I8);
					interp_ins_set_dreg (last_ins, sp [-1].local);
					WRITE64_INS (last_ins, 0, &ct);
				} else {
					interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_U4);
				}
				break;
			case StackType::I8:
				break;
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_U8_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_U8_R8);
				break;
			case StackType::MP:
#if SIZEOF_VOID_P == 4
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_U4);
#else
				SET_SIMPLE_TYPE(sp - 1, StackType::I8);
#endif
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CPOBJ: {
			CHECK_STACK (2);

			token = read32 (ip + 1);
			klass = mono_class_get_and_inflate_typespec_checked (image, token, generic_context, error);
			goto_if_nok (error, exit);

			if (m_class_is_valuetype (klass)) {
				MintType mt = mint_type (m_class_get_byval_arg (klass));
				sp -= 2;
				interp_add_ins ((mt == MintType::VT) ? MINT_CPOBJ_VT : MINT_CPOBJ);
				interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
				last_ins->data [0] = get_data_item_index (klass);
			} else {
				sp--;
				interp_add_ins (MINT_LDIND_REF);
				interp_ins_set_sreg (last_ins, sp [0].local);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp [-1].local);

				sp -= 2;
				interp_add_ins (MINT_STIND_REF);
				interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
			}
			ip += 5;
			break;
		}
		case CEE_LDOBJ: {
			CHECK_STACK (1);

			token = read32 (ip + 1);

			if (method->wrapper_type != MONO_WRAPPER_NONE)
				klass = (MonoClass *)mono_method_get_wrapper_data (method, token);
			else {
				klass = mono_class_get_and_inflate_typespec_checked (image, token, generic_context, error);
				goto_if_nok (error, exit);
			}

			interp_emit_ldobj (klass);

			ip += 5;
			BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_ACQ);
			break;
		}
		case CEE_LDSTR: {
			token = mono_metadata_token_index (read32 (ip + 1));
			push_type (StackType::O, mono_defaults.string_class);
			if (method->wrapper_type == MONO_WRAPPER_NONE) {
				MonoString *s = mono_ldstr_checked (domain, image, token, error);
				goto_if_nok (error, exit);
				/* GC won't scan code stream, but reference is held by metadata
				 * machinery so we are good here */
				interp_add_ins (MINT_LDSTR);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				last_ins->data [0] = get_data_item_index (s);
			} else {
				/* defer allocation to execution-time */
				interp_add_ins (MINT_LDSTR_TOKEN);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				last_ins->data [0] = get_data_item_index (GUINT_TO_POINTER (token));
			}
			ip += 5;
			break;
		}
		case CEE_NEWOBJ: {
			MonoMethod *m;
			MonoMethodSignature *csignature;

			ip++;
			token = read32 (ip);
			ip += 4;

			m = interp_get_method (method, token, image, generic_context, error);
			goto_if_nok (error, exit);

			csignature = mono_method_signature_internal (m);
			klass = m->klass;

			if (!mono_class_init_internal (klass)) {
				mono_error_set_for_class_failure (error, klass);
				goto_if_nok (error, exit);
			}

			if (mono_class_get_flags (klass) & TYPE_ATTRIBUTE_ABSTRACT) {
				char* full_name = mono_type_get_full_name (klass);
				mono_error_set_member_access (error, "Cannot create an abstract class: %s", full_name);
				g_free (full_name);
				goto_if_nok (error, exit);
			}

			MintType ret_mt = mint_type (m_class_get_byval_arg (klass));
			if (mono_class_is_magic_int (klass) || mono_class_is_magic_float (klass)) {
				g_assert (csignature->param_count == 1);
#if SIZEOF_VOID_P == 8
				if (mono_class_is_magic_int (klass) && sp [-1].type == StackType::I4)
					interp_add_conv (sp - 1, NULL, stack_type_of (ret_mt), MINT_CONV_I8_I4);
				else if (mono_class_is_magic_float (klass) && sp [-1].type == StackType::R4)
					interp_add_conv (sp - 1, NULL, stack_type_of (ret_mt), MINT_CONV_R8_R4);
#endif
			} else if (klass == mono_defaults.int_class && csignature->param_count == 1)  {
#if SIZEOF_VOID_P == 8
				if (sp [-1].type == StackType::I4)
					interp_add_conv (sp - 1, NULL, stack_type_of (ret_mt), MINT_CONV_I8_I4);
#else
				if (sp [-1].type == StackType::I8)
					interp_add_conv (sp - 1, NULL, stack_type_of (ret_mt), MINT_CONV_OVF_I4_I8);
#endif
			} else if (m_class_get_parent (klass) == mono_defaults.array_class) {
				sp -= csignature->param_count;
				for (int i = 0; i < csignature->param_count; i++)
					locals [sp [i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;

				interp_add_ins (MINT_NEWOBJ_ARRAY);
				last_ins->data [0] = get_data_item_index (m->klass);
				last_ins->data [1] = csignature->param_count;
				push_type (stack_type_of (ret_mt), klass);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				locals [sp [-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
			} else if (klass == mono_defaults.string_class) {
				guint32 tos_offset = get_tos_offset ();
				sp -= csignature->param_count;
				guint32 params_stack_size = tos_offset - get_tos_offset ();

				for (int i = 0; i < csignature->param_count; i++)
					locals [sp [i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;

				interp_add_ins (MINT_NEWOBJ_STRING);
				last_ins->data [0] = get_data_item_index (mono_interp_get_imethod (domain, m, error));
				last_ins->data [1] = params_stack_size;
				push_type (stack_type_of (ret_mt), klass);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				locals [sp [-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
			} else if (m_class_get_image (klass) == mono_defaults.corlib &&
					!strcmp (m_class_get_name (m->klass), "ByReference`1") &&
					!strcmp (m->name, ".ctor")) {
				/* public ByReference(ref T value) */
				g_assert (csignature->hasthis && csignature->param_count == 1);
				sp--;
				/* We already have the vt on top of the stack. Just do a dummy mov that should be optimized out */
				interp_add_ins (MINT_MOV_P);
				interp_ins_set_sreg (last_ins, sp [0].local);
				push_type_vt (klass, mono_class_value_size (klass, NULL));
				interp_ins_set_dreg (last_ins, sp [-1].local);
			} else if (m_class_get_image (klass) == mono_defaults.corlib &&
					(!strcmp (m_class_get_name (m->klass), "Span`1") ||
					!strcmp (m_class_get_name (m->klass), "ReadOnlySpan`1")) &&
					csignature->param_count == 2 &&
					csignature->params [0]->type == MONO_TYPE_PTR &&
					!type_has_references (mono_method_get_context (m)->class_inst->type_argv [0])) {
				/* ctor frequently used with ReadOnlySpan over static arrays */
				interp_add_ins (MINT_INTRINS_SPAN_CTOR);
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
				push_type_vt (klass, mono_class_value_size (klass, NULL));
				interp_ins_set_dreg (last_ins, sp [-1].local);
			} else {
				guint32 tos_offset = get_tos_offset ();
				sp -= csignature->param_count;
				guint32 params_stack_size = tos_offset - get_tos_offset ();

				// Move params types in temporary buffer
				// FIXME stop leaking sp_params
				StackInfo *sp_params = (StackInfo*) g_malloc (sizeof (StackInfo) * csignature->param_count);
				memcpy (sp_params, sp, sizeof (StackInfo) * csignature->param_count);

				// We must not optimize out these locals, storing to them is part of the interp call convention
				// FIXME this affects inlining efficiency. We need to first remove the param moving by NEWOBJ
				for (int i = 0; i < csignature->param_count; i++)
					locals [sp_params [i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;

				// Push the return value and `this` argument to the ctor
				gboolean is_vt = m_class_is_valuetype (klass);
				int vtsize = 0;
				if (is_vt) {
					vtsize = mono_class_value_size (klass, NULL);
					if (ret_mt == MintType::VT)
						push_type_vt (klass, vtsize);
					else
						push_type (stack_type_of (ret_mt), klass);
					push_simple_type (StackType::I);
				} else {
					push_type (stack_type_of (ret_mt), klass);
					push_type (stack_type_of (ret_mt), klass);
				}
				int dreg = sp [-2].local;
				locals [dreg].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;

				// Push back the params to top of stack
				push_types (sp_params, csignature->param_count);

				if (!mono_class_is_marshalbyref (klass) &&
						!mono_class_has_finalizer (klass) &&
						!m_class_has_weak_fields (klass)) {
					InterpInst *newobj_fast;

					if (is_vt) {
						newobj_fast = interp_add_ins (MINT_NEWOBJ_VT_FAST);
						interp_ins_set_dreg (newobj_fast, dreg);
						newobj_fast->data [1] = ALIGN_TO (vtsize, MINT_STACK_SLOT_SIZE);
					} else {
						MonoVTable *vtable = mono_class_vtable_checked (domain, klass, error);
						goto_if_nok (error, exit);
						newobj_fast = interp_add_ins (MINT_NEWOBJ_FAST);
						interp_ins_set_dreg (newobj_fast, dreg);
						newobj_fast->data [1] = get_data_item_index (vtable);
					}
					// FIXME remove these once we have our own local offset allocator, even for execution stack locals
					newobj_fast->data [2] = params_stack_size;
					newobj_fast->data [3] = csignature->param_count;

					if ((mono_interp_opt & INTERP_OPT_INLINE) && interp_method_check_inlining (m, csignature)) {
						MonoMethodHeader *mheader = interp_method_get_header (m, error);
						goto_if_nok (error, exit);

						// Add local mapping information for cprop to use, in case we inline
						int param_count = csignature->param_count;
						int *newobj_reg_map = arena.create_array<int> (param_count * 2);
						for (int i = 0; i < param_count; i++) {
							newobj_reg_map [2 * i] = sp_params [i].local;
							newobj_reg_map [2 * i + 1] = sp [-param_count + i].local;
						}

						if (interp_inline_method (m, mheader, error)) {
							newobj_fast->data [0] = INLINED_METHOD_FLAG;
							newobj_fast->info.newobj_reg_map = newobj_reg_map;
							break;
						}
					}
					// Inlining failed. Set the method to be executed as part of newobj instruction
					newobj_fast->data [0] = get_data_item_index (mono_interp_get_imethod (domain, m, error));
					/* The constructor was not inlined, abort inlining of current method */
					INLINE_FAILURE;
				} else {
					interp_add_ins (MINT_NEWOBJ);
					g_assert (!m_class_is_valuetype (klass));
					interp_ins_set_dreg (last_ins, dreg);
					last_ins->data [0] = get_data_item_index (mono_interp_get_imethod (domain, m, error));
					last_ins->data [1] = params_stack_size;
				}
				goto_if_nok (error, exit);
				// Parameters and this pointer are popped of the stack. The return value remains
				sp -= csignature->param_count + 1;
			}
			break;
		}
		case CEE_CASTCLASS:
		case CEE_ISINST: {
			gboolean isinst_instr = *ip == CEE_ISINST;
			CHECK_STACK (1);
			token = read32 (ip + 1);
			klass = mini_get_class (method, token, generic_context);
			CHECK_TYPELOAD (klass);
			interp_handle_isinst (klass, isinst_instr);
			break;
		}
		case CEE_CONV_R_UN:
			switch (sp [-1].type) {
			case StackType::R8:
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R_UN_I8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R_UN_I4);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_UNBOX:
			CHECK_STACK (1);
			token = read32 (ip + 1);
			
			if (method->wrapper_type != MONO_WRAPPER_NONE)
				klass = (MonoClass *)mono_method_get_wrapper_data (method, token);
			else {
				klass = mono_class_get_and_inflate_typespec_checked (image, token, generic_context, error);
				goto_if_nok (error, exit);
			}

			if (mono_class_is_nullable (klass)) {
				MonoMethod *target_method;
				if (m_class_is_enumtype (mono_class_get_nullable_param_internal (klass)))
					target_method = mono_class_get_method_from_name_checked (klass, "UnboxExact", 1, 0, error);
				else
					target_method = mono_class_get_method_from_name_checked (klass, "Unbox", 1, 0, error);
				goto_if_nok (error, exit);
				/* ip is incremented by interp_transform_call */
				if (!interp_transform_call (method, target_method, domain, generic_context, NULL, FALSE, error, FALSE, FALSE, FALSE))
					goto exit;
				/*
				 * CEE_UNBOX needs to push address of vtype while Nullable.Unbox returns the value type
				 * We create a local variable in the frame so that we can fetch its address.
				 */
				int local = create_interp_local (m_class_get_byval_arg (klass));
				store_local (local);

				interp_add_ins (MINT_LDLOCA_S);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				interp_ins_set_sreg (last_ins, local);
				locals [local].indirects++;
			} else {
				interp_add_ins (MINT_UNBOX);
				sp--;
				interp_ins_set_sreg (last_ins, sp [0].local);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				last_ins->data [0] = get_data_item_index (klass);
				ip += 5;
			}
			break;
		case CEE_UNBOX_ANY:
			CHECK_STACK (1);
			token = read32 (ip + 1);

			klass = mini_get_class (method, token, generic_context);
			CHECK_TYPELOAD (klass);

			// Common in generic code:
			// box T + unbox.any T -> nop
			if ((last_ins->opcode == MINT_BOX || last_ins->opcode == MINT_BOX_VT) &&
					(sp - 1)->klass == klass && last_ins == cbb->last_ins) {
				interp_clear_ins (last_ins);
				MintType mt = mint_type (m_class_get_byval_arg (klass));
				sp--;
				// Push back the original value that was boxed. We should handle this in CEE_BOX instead
				if (mt == MintType::VT)
					push_type_vt (klass, mono_class_value_size (klass, NULL));
				else
					push_type (stack_type_of (mt), klass);
				// FIXME do this somewhere else, maybe in super instruction pass, where we would check
				// instruction patterns
				// Restore the local that is on top of the stack
				sp [-1].local = last_ins->sregs [0];
				ip += 5;
				break;
			}

			if (mini_type_is_reference (m_class_get_byval_arg (klass))) {
				interp_handle_isinst (klass, FALSE);
			} else if (mono_class_is_nullable (klass)) {
				MonoMethod *target_method;
				if (m_class_is_enumtype (mono_class_get_nullable_param_internal (klass)))
					target_method = mono_class_get_method_from_name_checked (klass, "UnboxExact", 1, 0, error);
				else
					target_method = mono_class_get_method_from_name_checked (klass, "Unbox", 1, 0, error);
				goto_if_nok (error, exit);
				/* ip is incremented by interp_transform_call */
				if (!interp_transform_call (method, target_method, domain, generic_context, NULL, FALSE, error, FALSE, FALSE, FALSE))
					goto exit;
			} else {
				interp_add_ins (MINT_UNBOX);
				sp--;
				interp_ins_set_sreg (last_ins, sp [0].local);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				last_ins->data [0] = get_data_item_index (klass);

				interp_emit_ldobj (klass);

				ip += 5;
			}

			break;
		case CEE_THROW:
			INLINE_FAILURE;
			CHECK_STACK (1);
			interp_add_ins (MINT_THROW);
			interp_ins_set_sreg (last_ins, sp [-1].local);
			link_bblocks = FALSE;
			sp = stack;
			++ip;
			break;
		case CEE_LDFLDA: {
			CHECK_STACK (1);
			token = read32 (ip + 1);
			field = interp_field_from_token (method, token, &klass, generic_context, error);
			goto_if_nok (error, exit);
			MonoType *ftype = mono_field_get_type_internal (field);
			gboolean is_static = !!(ftype->attrs & FIELD_ATTRIBUTE_STATIC);
			mono_class_init_internal (klass);
#ifndef DISABLE_REMOTING
			if (m_class_get_marshalbyref (klass) || mono_class_is_contextbound (klass) || klass == mono_defaults.marshalbyrefobject_class) {
				g_assert (!is_static);
				int offset = m_class_is_valuetype (klass) ? field->offset - MONO_ABI_SIZEOF (MonoObject) : field->offset;

				interp_add_ins (MINT_MONO_LDPTR);
				last_ins->data [0] = get_data_item_index (klass);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp [-1].local);

				interp_add_ins (MINT_MONO_LDPTR);
				last_ins->data [0] = get_data_item_index (field);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp [-1].local);

				interp_add_ins (MINT_LDC_I4);
				WRITE32_INS (last_ins, 0, &offset);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp [-1].local);
#if SIZEOF_VOID_P == 8
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_I4);
#endif

				MonoMethod *wrapper = mono_marshal_get_ldflda_wrapper (field->type);
				/* ip is incremented by interp_transform_call */
				if (!interp_transform_call (method, wrapper, domain, generic_context, NULL, FALSE, error, FALSE, FALSE, FALSE))
					goto exit;
			} else
#endif
			{
				if (is_static) {
					sp--;
					interp_emit_ldsflda (field, error);
					goto_if_nok (error, exit);
				} else {
					sp--;
					if (sp->type == StackType::O) {
						interp_add_ins (MINT_LDFLDA);
					} else {
						StackType sp_type = sp->type;
						g_assert (sp_type == StackType::MP || sp_type == StackType::I);
						interp_add_ins (MINT_LDFLDA_UNSAFE);
					}
					last_ins->data [0] = m_class_is_valuetype (klass) ? field->offset - MONO_ABI_SIZEOF (MonoObject) : field->offset;
					interp_ins_set_sreg (last_ins, sp [0].local);
					push_simple_type (StackType::MP);
					interp_ins_set_dreg (last_ins, sp [-1].local);
				}
				ip += 5;
			}
			break;
		}
		case CEE_LDFLD: {
			CHECK_STACK (1);
			token = read32 (ip + 1);
			field = interp_field_from_token (method, token, &klass, generic_context, error);
			goto_if_nok (error, exit);
			MonoType *ftype = mono_field_get_type_internal (field);
			gboolean is_static = !!(ftype->attrs & FIELD_ATTRIBUTE_STATIC);
			mono_class_init_internal (klass);

			MonoClass *field_klass = mono_class_from_mono_type_internal (ftype);
			mt = mint_type (m_class_get_byval_arg (field_klass));
			int field_size = mono_class_value_size (field_klass, NULL);
			int obj_size = mono_class_value_size (klass, NULL);
			obj_size = ALIGN_TO (obj_size, MINT_VT_ALIGNMENT);

#ifndef DISABLE_REMOTING
			if (m_class_get_marshalbyref (klass) ||
					mono_class_is_contextbound (klass) ||
					klass == mono_defaults.marshalbyrefobject_class) {
				g_assert (!is_static);
				interp_add_ins (mt == MintType::VT ? MINT_LDRMFLD_VT :  MINT_LDRMFLD);
				sp--;
				interp_ins_set_sreg (last_ins, sp [0].local);
				last_ins->data [0] = get_data_item_index (field);
				if (mt == MintType::VT)
					push_type_vt (field_klass, field_size);
				else
					push_type (stack_type_of (mt), field_klass);
				interp_ins_set_dreg (last_ins, sp [-1].local);
			} else
#endif
			{
				if (is_static) {
					sp--;
					interp_emit_sfld_access (field, field_klass, mt, TRUE, error);
					goto_if_nok (error, exit);
				} else if (sp [-1].type == StackType::VT) {
					/* First we pop the vt object from the stack. Then we push the field */
					int opcode = op_for_mint_type (MINT_LDFLD_VT_I1, mt);
#ifdef NO_UNALIGNED_ACCESS
					if (field->offset % SIZEOF_VOID_P != 0) {
						if (mt == MintType::I8)
							opcode = MINT_LDFLD_VT_I8_UNALIGNED;
						else if (mt == MintType::R8)
							opcode = MINT_LDFLD_VT_R8_UNALIGNED;
					}
#endif
					interp_add_ins (opcode);
					g_assert (m_class_is_valuetype (klass));
					sp--;
					interp_ins_set_sreg (last_ins, sp [0].local);
					last_ins->data [0] = field->offset - MONO_ABI_SIZEOF (MonoObject);
					if (mt == MintType::VT)
						last_ins->data [1] = field_size;
					if (mt == MintType::VT)
						push_type_vt (field_klass, field_size);
					else
						push_type (stack_type_of (mt), field_klass);
					interp_ins_set_dreg (last_ins, sp [-1].local);
				} else {
					int opcode = op_for_mint_type (MINT_LDFLD_I1, mt);
#ifdef NO_UNALIGNED_ACCESS
					if ((mt == MintType::I8 || mt == MintType::R8) && field->offset % SIZEOF_VOID_P != 0)
						opcode = get_unaligned_opcode (opcode);
#endif
					interp_add_ins (opcode);
					sp--;
					interp_ins_set_sreg (last_ins, sp [0].local);
					last_ins->data [0] = m_class_is_valuetype (klass) ? field->offset - MONO_ABI_SIZEOF (MonoObject) : field->offset;
					if (mt == MintType::VT) {
						int size = mono_class_value_size (field_klass, NULL);
						g_assert (size < G_MAXUINT16);
						last_ins->data [1] = size;
					}
					if (mt == MintType::VT)
						push_type_vt (field_klass, field_size);
					else
						push_type (stack_type_of (mt), field_klass);
					interp_ins_set_dreg (last_ins, sp [-1].local);
				}
			}
			ip += 5;
			BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_ACQ);
			break;
		}
		case CEE_STFLD: {
			CHECK_STACK (2);
			token = read32 (ip + 1);
			field = interp_field_from_token (method, token, &klass, generic_context, error);
			goto_if_nok (error, exit);
			MonoType *ftype = mono_field_get_type_internal (field);
			gboolean is_static = !!(ftype->attrs & FIELD_ATTRIBUTE_STATIC);
			MonoClass *field_klass = mono_class_from_mono_type_internal (ftype);
			mono_class_init_internal (klass);
			mt = mint_type (ftype);

			emit_convert (ftype);

			BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_REL);

#ifndef DISABLE_REMOTING
			if (m_class_get_marshalbyref (klass)) {
				g_assert (!is_static);
				interp_add_ins (mt == MintType::VT ? MINT_STRMFLD_VT : MINT_STRMFLD);
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
				last_ins->data [0] = get_data_item_index (field);
			} else
#endif
			{
				if (is_static) {
					interp_emit_sfld_access (field, field_klass, mt, FALSE, error);
					goto_if_nok (error, exit);

					/* pop the unused object reference */
					sp--;

					/* the vtable of the field might not be initialized at this point */
					mono_class_vtable_checked (domain, field_klass, error);
					goto_if_nok (error, exit);
				} else {
					int opcode = op_for_mint_type (MINT_STFLD_I1, mt);
#ifdef NO_UNALIGNED_ACCESS
					if ((mt == MintType::I8 || mt == MintType::R8) && field->offset % SIZEOF_VOID_P != 0)
						opcode = get_unaligned_opcode (opcode);
#endif
					interp_add_ins (opcode);
					sp -= 2;
					interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
					last_ins->data [0] = m_class_is_valuetype (klass) ? field->offset - MONO_ABI_SIZEOF (MonoObject) : field->offset;
					if (mt == MintType::VT) {
						/* the vtable of the field might not be initialized at this point */
						mono_class_vtable_checked (domain, field_klass, error);
						goto_if_nok (error, exit);
						if (m_class_has_references (field_klass)) {
							last_ins->data [1] = get_data_item_index (field_klass);
						} else {
							last_ins->opcode = MINT_STFLD_VT_NOREF;
							last_ins->data [1] = mono_class_value_size (field_klass, NULL);
						}
					}
				}
			}
			ip += 5;
			break;
		}
		case CEE_LDSFLDA: {
			token = read32 (ip + 1);
			field = interp_field_from_token (method, token, &klass, generic_context, error);
			goto_if_nok (error, exit);
			interp_emit_ldsflda (field, error);
			goto_if_nok (error, exit);
			ip += 5;
			break;
		}
		case CEE_LDSFLD: {
			token = read32 (ip + 1);
			field = interp_field_from_token (method, token, &klass, generic_context, error);
			goto_if_nok (error, exit);
			MonoType *ftype = mono_field_get_type_internal (field);
			mt = mint_type (ftype);
			klass = mono_class_from_mono_type_internal (ftype);
			gboolean in_corlib = m_class_get_image (field->parent) == mono_defaults.corlib;

			if (in_corlib && !strcmp (field->name, "IsLittleEndian") &&
				!strcmp (m_class_get_name (field->parent), "BitConverter") &&
				!strcmp (m_class_get_name_space (field->parent), "System"))
			{
				interp_add_ins ((TARGET_BYTE_ORDER == G_LITTLE_ENDIAN) ? MINT_LDC_I4_1 : MINT_LDC_I4_0);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				ip += 5;
				break;
			}

			interp_emit_sfld_access (field, klass, mt, TRUE, error);
			goto_if_nok (error, exit);

			ip += 5;
			break;
		}
		case CEE_STSFLD: {
			CHECK_STACK (1);
			token = read32 (ip + 1);
			field = interp_field_from_token (method, token, &klass, generic_context, error);
			goto_if_nok (error, exit);
			MonoType *ftype = mono_field_get_type_internal (field);
			mt = mint_type (ftype);

			emit_convert (ftype);

			/* the vtable of the field might not be initialized at this point */
			MonoClass *fld_klass = mono_class_from_mono_type_internal (ftype);
			mono_class_vtable_checked (domain, fld_klass, error);
			goto_if_nok (error, exit);

			interp_emit_sfld_access (field, fld_klass, mt, FALSE, error);
			goto_if_nok (error, exit);

			ip += 5;
			break;
		}
		case CEE_STOBJ: {
			token = read32 (ip + 1);

			if (method->wrapper_type != MONO_WRAPPER_NONE)
				klass = (MonoClass *)mono_method_get_wrapper_data (method, token);
			else
				klass = mini_get_class (method, token, generic_context);
			CHECK_TYPELOAD (klass);

			BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_REL);

			interp_emit_stobj (klass);

			ip += 5;
			break;
		}
		case CEE_CONV_OVF_I_UN:
		case CEE_CONV_OVF_U_UN: {
			CHECK_STACK (1);
			gboolean to_signed = *ip == CEE_CONV_OVF_I_UN;
			int op = -1;

			switch (sp [-1].type) {
			/*
			 * ECMA-335 III.3.29 gives .un no reading on a float source, so these
			 * check the destination's own range, as the form without .un does.
			 */
			case StackType::R4:
#if SIZEOF_VOID_P == 8
				op = to_signed ? MINT_CONV_OVF_I8_R4 : MINT_CONV_OVF_U8_R4;
#else
				op = to_signed ? MINT_CONV_OVF_I4_R4 : MINT_CONV_OVF_U4_R4;
#endif
				break;
			case StackType::R8:
#if SIZEOF_VOID_P == 8
				op = to_signed ? MINT_CONV_OVF_I8_R8 : MINT_CONV_OVF_U8_R8;
#else
				op = to_signed ? MINT_CONV_OVF_I4_R8 : MINT_CONV_OVF_U4_R8;
#endif
				break;
			case StackType::I8:
#if SIZEOF_VOID_P == 8
				/* Read unsigned, an int64 reaches past what a signed native int holds. */
				if (to_signed)
					op = MINT_CONV_OVF_I8_U8;
#else
				op = MINT_CONV_OVF_I4_U8;
#endif
				break;
			case StackType::I4:
#if SIZEOF_VOID_P == 8
				op = MINT_CONV_I8_U4;
#else
				if (to_signed)
					op = MINT_CONV_OVF_I4_U4;
#endif
				break;
			default:
				g_assert_not_reached ();
				break;
			}

			if (op != -1)
				interp_add_conv (sp - 1, NULL, StackType::I, op);
			++ip;
			break;
		}
		case CEE_CONV_OVF_I8_UN:
		case CEE_CONV_OVF_U8_UN: {
			CHECK_STACK (1);
			gboolean to_signed = *ip == CEE_CONV_OVF_I8_UN;
			int op = -1;

			switch (sp [-1].type) {
			/* The float source reads the same way it does for the native int forms. */
			case StackType::R4:
				op = to_signed ? MINT_CONV_OVF_I8_R4 : MINT_CONV_OVF_U8_R4;
				break;
			case StackType::R8:
				op = to_signed ? MINT_CONV_OVF_I8_R8 : MINT_CONV_OVF_U8_R8;
				break;
			case StackType::I8:
				if (to_signed)
					op = MINT_CONV_OVF_I8_U8;
				break;
			case StackType::I4:
				op = MINT_CONV_I8_U4;
				break;
			default:
				g_assert_not_reached ();
				break;
			}

			if (op != -1)
				interp_add_conv (sp - 1, NULL, StackType::I8, op);
			++ip;
			break;
		}
		case CEE_BOX: {
			CHECK_STACK (1);
			token = read32 (ip + 1);
			if (method->wrapper_type != MONO_WRAPPER_NONE)
				klass = (MonoClass *)mono_method_get_wrapper_data (method, token);
			else
				klass = mini_get_class (method, token, generic_context);
			CHECK_TYPELOAD (klass);

			if (mono_class_is_nullable (klass)) {
				MonoMethod *target_method = mono_class_get_method_from_name_checked (klass, "Box", 1, 0, error);
				goto_if_nok (error, exit);
				/* ip is incremented by interp_transform_call */
				if (!interp_transform_call (method, target_method, domain, generic_context, NULL, FALSE, error, FALSE, FALSE, FALSE))
					goto exit;
			} else if (!m_class_is_valuetype (klass)) {
				/* already boxed, do nothing. */
				ip += 5;
			} else {
				if (G_UNLIKELY (m_class_is_byreflike (klass))) {
					mono_error_set_bad_image (error, image, "Cannot box IsByRefLike type '%s.%s'", m_class_get_name_space (klass), m_class_get_name (klass));
					goto exit;
				}

				const MintType boxed_mt = mint_type (m_class_get_byval_arg (klass));
				const gboolean vt = boxed_mt == MintType::VT;

				coerce_fp (sp - 1, stack_type_of (boxed_mt));
				MonoVTable *vtable = mono_class_vtable_checked (domain, klass, error);
				goto_if_nok (error, exit);

				sp--;
				interp_add_ins (vt ? MINT_BOX_VT : MINT_BOX);
				interp_ins_set_sreg (last_ins, sp [0].local);
				last_ins->data [0] = get_data_item_index (vtable);
				push_type (StackType::O, klass);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				ip += 5;
			}

			break;
		}
		case CEE_NEWARR: {
			CHECK_STACK (1);
			token = read32 (ip + 1);

			if (method->wrapper_type != MONO_WRAPPER_NONE)
				klass = (MonoClass *)mono_method_get_wrapper_data (method, token);
			else
				klass = mini_get_class (method, token, generic_context);
			CHECK_TYPELOAD (klass);

			MonoClass *array_class = mono_class_create_array (klass, 1);
			MonoVTable *vtable = mono_class_vtable_checked (domain, array_class, error);
			goto_if_nok (error, exit);

			StackType lentype = (sp - 1)->type;
			if (lentype == StackType::I8) {
				/* mimic mini behaviour */
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_I8);
			} else {
				g_assert (lentype == StackType::I4);
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_I4);
			}
			sp--;
			interp_add_ins (MINT_NEWARR);
			interp_ins_set_sreg (last_ins, sp [0].local);
			push_type (StackType::O, array_class);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			last_ins->data [0] = get_data_item_index (vtable);
			ip += 5;
			break;
		}
		case CEE_LDLEN:
			CHECK_STACK (1);
			sp--;
			interp_add_ins (MINT_LDLEN);
			interp_ins_set_sreg (last_ins, sp [0].local);
#ifdef MONO_BIG_ARRAYS
			push_simple_type (StackType::I8);
#else
			push_simple_type (StackType::I4);
#endif
			interp_ins_set_dreg (last_ins, sp [-1].local);
			++ip;
			break;
		case CEE_LDELEMA: {
			gint32 size;
			CHECK_STACK (2);
			narrow_index (sp - 1);
			token = read32 (ip + 1);

			if (method->wrapper_type != MONO_WRAPPER_NONE)
				klass = (MonoClass *) mono_method_get_wrapper_data (method, token);
			else
				klass = mini_get_class (method, token, generic_context);

			CHECK_TYPELOAD (klass);

			if (!m_class_is_valuetype (klass) && method->wrapper_type == MONO_WRAPPER_NONE && !readonly) {
				/*
				 * Check the class for failures before the type check, which can
				 * throw other exceptions.
				 */
				mono_class_setup_vtable (klass);
				CHECK_TYPELOAD (klass);
				interp_add_ins (MINT_LDELEMA_TC);
				sp -= 2;
				locals [sp [0].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
				locals [sp [1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				locals [sp [-1].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
				last_ins->data [0] = get_data_item_index (klass);
			} else {
				interp_add_ins (MINT_LDELEMA1);
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				mono_class_init_internal (klass);
				size = mono_class_array_element_size (klass);
				last_ins->data [0] = size;
			}

			readonly = FALSE;

			ip += 5;
			break;
		}
		case CEE_LDELEM_I1:
			handle_ldelem (MINT_LDELEM_I1, StackType::I4);
			break;
		case CEE_LDELEM_U1:
			handle_ldelem (MINT_LDELEM_U1, StackType::I4);
			break;
		case CEE_LDELEM_I2:
			handle_ldelem (MINT_LDELEM_I2, StackType::I4);
			break;
		case CEE_LDELEM_U2:
			handle_ldelem (MINT_LDELEM_U2, StackType::I4);
			break;
		case CEE_LDELEM_I4:
			handle_ldelem (MINT_LDELEM_I4, StackType::I4);
			break;
		case CEE_LDELEM_U4:
			handle_ldelem (MINT_LDELEM_U4, StackType::I4);
			break;
		case CEE_LDELEM_I8:
			handle_ldelem (MINT_LDELEM_I8, StackType::I8);
			break;
		case CEE_LDELEM_I:
			handle_ldelem (MINT_LDELEM_I, StackType::I);
			break;
		case CEE_LDELEM_R4:
			handle_ldelem (MINT_LDELEM_R4, StackType::R4);
			break;
		case CEE_LDELEM_R8:
			handle_ldelem (MINT_LDELEM_R8, StackType::R8);
			break;
		case CEE_LDELEM_REF:
			handle_ldelem (MINT_LDELEM_REF, StackType::O);
			break;
		case CEE_LDELEM:
			token = read32 (ip + 1);
			klass = mini_get_class (method, token, generic_context);
			CHECK_TYPELOAD (klass);
			switch (mint_type (m_class_get_byval_arg (klass))) {
				case MintType::I1:
					handle_ldelem (MINT_LDELEM_I1, StackType::I4);
					break;
				case MintType::U1:
					handle_ldelem (MINT_LDELEM_U1, StackType::I4);
					break;
				case MintType::U2:
					handle_ldelem (MINT_LDELEM_U2, StackType::I4);
					break;
				case MintType::I2:
					handle_ldelem (MINT_LDELEM_I2, StackType::I4);
					break;
				case MintType::I4:
					handle_ldelem (MINT_LDELEM_I4, StackType::I4);
					break;
				case MintType::I8:
					handle_ldelem (MINT_LDELEM_I8, StackType::I8);
					break;
				case MintType::R4:
					handle_ldelem (MINT_LDELEM_R4, StackType::R4);
					break;
				case MintType::R8:
					handle_ldelem (MINT_LDELEM_R8, StackType::R8);
					break;
				case MintType::O:
					handle_ldelem (MINT_LDELEM_REF, StackType::O);
					break;
				case MintType::VT: {
					int size = mono_class_value_size (klass, NULL);
					g_assert (size < G_MAXUINT16);

					CHECK_STACK (2);
					narrow_index (sp - 1);
					interp_add_ins (MINT_LDELEM_VT);
					sp -= 2;
					interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
					push_type_vt (klass, size);
					interp_ins_set_dreg (last_ins, sp [-1].local);
					last_ins->data [0] = size;
					++ip;
					break;
				}
				default: {
					GString *res = g_string_new ("");
					mono_type_get_desc (res, m_class_get_byval_arg (klass), TRUE);
					g_print ("LDELEM: %s -> %d (%s)\n", m_class_get_name (klass), mint_type (m_class_get_byval_arg (klass)), res->str);
					g_string_free (res, TRUE);
					g_assert (0);
					break;
				}
			}
			ip += 4;
			break;
		case CEE_STELEM_I:
			handle_stelem (MINT_STELEM_I);
			break;
		case CEE_STELEM_I1:
			handle_stelem (MINT_STELEM_I1);
			break;
		case CEE_STELEM_I2:
			handle_stelem (MINT_STELEM_I2);
			break;
		case CEE_STELEM_I4:
			handle_stelem (MINT_STELEM_I4);
			break;
		case CEE_STELEM_I8:
			handle_stelem (MINT_STELEM_I8);
			break;
		case CEE_STELEM_R4:
			handle_stelem (MINT_STELEM_R4);
			break;
		case CEE_STELEM_R8:
			handle_stelem (MINT_STELEM_R8);
			break;
		case CEE_STELEM_REF:
			handle_stelem (MINT_STELEM_REF);
			break;
		case CEE_STELEM:
			token = read32 (ip + 1);
			klass = mini_get_class (method, token, generic_context);
			CHECK_TYPELOAD (klass);
			switch (mint_type (m_class_get_byval_arg (klass))) {
				case MintType::I1:
					handle_stelem (MINT_STELEM_I1);
					break;
				case MintType::U1:
					handle_stelem (MINT_STELEM_U1);
					break;
				case MintType::I2:
					handle_stelem (MINT_STELEM_I2);
					break;
				case MintType::U2:
					handle_stelem (MINT_STELEM_U2);
					break;
				case MintType::I4:
					handle_stelem (MINT_STELEM_I4);
					break;
				case MintType::I8:
					handle_stelem (MINT_STELEM_I8);
					break;
				case MintType::R4:
					handle_stelem (MINT_STELEM_R4);
					break;
				case MintType::R8:
					handle_stelem (MINT_STELEM_R8);
					break;
				case MintType::O:
					handle_stelem (MINT_STELEM_REF);
					break;
				case MintType::VT: {
					int size = mono_class_value_size (klass, NULL);
					g_assert (size < G_MAXUINT16);

					handle_stelem (MINT_STELEM_VT);
					last_ins->data [0] = get_data_item_index (klass);
					last_ins->data [1] = size;
					break;
				}
				default: {
					GString *res = g_string_new ("");
					mono_type_get_desc (res, m_class_get_byval_arg (klass), TRUE);
					g_print ("STELEM: %s -> %d (%s)\n", m_class_get_name (klass), mint_type (m_class_get_byval_arg (klass)), res->str);
					g_string_free (res, TRUE);
					g_assert (0);
					break;
				}
			}
			ip += 4;
			break;
		case CEE_CKFINITE: {
			CHECK_STACK (1);
			// ckfinite hands its operand back, so the value keeps the width it
			// arrived with. Reading a single as a double gets four bytes of
			// whatever follows it.
			StackType float_type = sp [-1].type == StackType::R4 ? StackType::R4
			                                                   : StackType::R8;
			interp_add_ins (float_type == StackType::R4 ? MINT_CKFINITE_R4
			                                                : MINT_CKFINITE);
			sp--;
			interp_ins_set_sreg (last_ins, sp [0].local);
			push_simple_type (float_type);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			++ip;
			break;
		}
		case CEE_MKREFANY:
			CHECK_STACK (1);

			token = read32 (ip + 1);
			klass = mini_get_class (method, token, generic_context);
			CHECK_TYPELOAD (klass);

			interp_add_ins (MINT_MKREFANY);
			sp--;
			interp_ins_set_sreg (last_ins, sp [0].local);
			push_type_vt (mono_defaults.typed_reference_class, sizeof (MonoTypedRef));
			interp_ins_set_dreg (last_ins, sp [-1].local);
			last_ins->data [0] = get_data_item_index (klass);

			ip += 5;
			break;
		case CEE_REFANYVAL: {
			CHECK_STACK (1);

			token = read32 (ip + 1);
			klass = mini_get_class (method, token, generic_context);
			CHECK_TYPELOAD (klass);

			interp_add_ins (MINT_REFANYVAL);
			sp--;
			interp_ins_set_sreg (last_ins, sp [0].local);
			push_simple_type (StackType::MP);
			interp_ins_set_dreg (last_ins, sp [-1].local);
			last_ins->data [0] = get_data_item_index (klass);

			ip += 5;
			break;
		}
		case CEE_CONV_OVF_I1:
		case CEE_CONV_OVF_I1_UN: {
			gboolean is_un = *ip == CEE_CONV_OVF_I1_UN;
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, is_un ? MINT_CONV_OVF_I1_UN_R4 : MINT_CONV_OVF_I1_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, is_un ? MINT_CONV_OVF_I1_UN_R8 : MINT_CONV_OVF_I1_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, is_un ? MINT_CONV_OVF_I1_U4 : MINT_CONV_OVF_I1_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, is_un ? MINT_CONV_OVF_I1_U8 : MINT_CONV_OVF_I1_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		}
		case CEE_CONV_OVF_U1:
		case CEE_CONV_OVF_U1_UN:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U1_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U1_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U1_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U1_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_CONV_OVF_I2:
		case CEE_CONV_OVF_I2_UN: {
			gboolean is_un = *ip == CEE_CONV_OVF_I2_UN;
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, is_un ? MINT_CONV_OVF_I2_UN_R4 : MINT_CONV_OVF_I2_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, is_un ? MINT_CONV_OVF_I2_UN_R8 : MINT_CONV_OVF_I2_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, is_un ? MINT_CONV_OVF_I2_U4 : MINT_CONV_OVF_I2_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, is_un ? MINT_CONV_OVF_I2_U8 : MINT_CONV_OVF_I2_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		}
		case CEE_CONV_OVF_U2_UN:
		case CEE_CONV_OVF_U2:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U2_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U2_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U2_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U2_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
#if SIZEOF_VOID_P == 4
		case CEE_CONV_OVF_I:
#endif
		case CEE_CONV_OVF_I4:
		case CEE_CONV_OVF_I4_UN:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_I4_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_I4_R8);
				break;
			case StackType::I4:
				if (*ip == CEE_CONV_OVF_I4_UN)
					interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_I4_U4);
				break;
			case StackType::I8:
				if (*ip == CEE_CONV_OVF_I4_UN)
					interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_I4_U8);
				else
					interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_I4_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
#if SIZEOF_VOID_P == 4
		case CEE_CONV_OVF_U:
#endif
		case CEE_CONV_OVF_U4:
		case CEE_CONV_OVF_U4_UN:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_R8);
				break;
			case StackType::I4:
				if (*ip != CEE_CONV_OVF_U4_UN)
					interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_I8);
				break;
			case StackType::MP:
				interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_OVF_U4_P);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
#if SIZEOF_VOID_P == 8
		case CEE_CONV_OVF_I:
#endif
		case CEE_CONV_OVF_I8:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_OVF_I8_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_OVF_I8_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_I8_I4);
				break;
			case StackType::I8:
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
#if SIZEOF_VOID_P == 8
		case CEE_CONV_OVF_U:
#endif
		case CEE_CONV_OVF_U8:
			CHECK_STACK (1);
			switch (sp [-1].type) {
			case StackType::R4:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_OVF_U8_R4);
				break;
			case StackType::R8:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_OVF_U8_R8);
				break;
			case StackType::I4:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_OVF_U8_I4);
				break;
			case StackType::I8:
				interp_add_conv (sp - 1, NULL, StackType::I8, MINT_CONV_OVF_U8_I8);
				break;
			default:
				g_assert_not_reached ();
			}
			++ip;
			break;
		case CEE_LDTOKEN: {
			int size;
			gpointer handle;
			token = read32 (ip + 1);
			if (method->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD || method->wrapper_type == MONO_WRAPPER_SYNCHRONIZED) {
				handle = mono_method_get_wrapper_data (method, token);
				klass = (MonoClass *) mono_method_get_wrapper_data (method, token + 1);
				if (klass == mono_defaults.typehandle_class)
					handle = m_class_get_byval_arg ((MonoClass *) handle);

				if (generic_context) {
					handle = mono_class_inflate_generic_type_checked ((MonoType*)handle, generic_context, error);
					goto_if_nok (error, exit);
				}
			} else {
				handle = mono_ldtoken_checked (image, token, &klass, generic_context, error);
				goto_if_nok (error, exit);
			}
			mono_class_init_internal (klass);
			mt = mint_type (m_class_get_byval_arg (klass));
			g_assert (mt == MintType::VT);
			size = mono_class_value_size (klass, NULL);
			g_assert (size == sizeof(gpointer));

			const unsigned char *next_ip = ip + 5;
			MonoMethod *cmethod;
			if (next_ip < end &&
					interp_ip_in_cbb (next_ip - il_code) &&
					(*next_ip == CEE_CALL || *next_ip == CEE_CALLVIRT) &&
					(cmethod = interp_get_method (method, read32 (next_ip + 1), image, generic_context, error)) &&
					(cmethod->klass == mono_defaults.systemtype_class) &&
					(strcmp (cmethod->name, "GetTypeFromHandle") == 0)) {
				const unsigned char *next_next_ip = next_ip + 5;
				MonoMethod *next_cmethod;
				MonoClass *tclass = mono_class_from_mono_type_internal ((MonoType *)handle);
				// Optimize to true/false if next instruction is `call instance bool Type::get_IsValueType()`
				if (next_next_ip < end &&
						interp_ip_in_cbb (next_next_ip - il_code) &&
						(*next_next_ip == CEE_CALL || *next_next_ip == CEE_CALLVIRT) &&
						(next_cmethod = interp_get_method (method, read32 (next_next_ip + 1), image, generic_context, error)) &&
						(next_cmethod->klass == mono_defaults.systemtype_class) &&
						!strcmp (next_cmethod->name, "get_IsValueType")) {
					g_assert (!mono_class_is_open_constructed_type (m_class_get_byval_arg (tclass)));
					if (m_class_is_valuetype (tclass))
						interp_add_ins (MINT_LDC_I4_1);
					else
						interp_add_ins (MINT_LDC_I4_0);
					push_simple_type (StackType::I4);
					interp_ins_set_dreg (last_ins, sp [-1].local);
					ip = next_next_ip + 5;
					break;
				}

				interp_add_ins (MINT_MONO_LDPTR);
				gpointer systype = mono_type_get_object_checked (domain, (MonoType*)handle, error);
				goto_if_nok (error, exit);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				last_ins->data [0] = get_data_item_index (systype);
				ip = next_ip + 5;
			} else {
				interp_add_ins (MINT_LDTOKEN);
				push_type_vt (klass, sizeof (gpointer));
				interp_ins_set_dreg (last_ins, sp [-1].local);
				last_ins->data [0] = get_data_item_index (handle);
				ip += 5;
			}

			break;
		}
		case CEE_ADD_OVF:
			binary_arith_op (MINT_ADD_OVF_I4);
			++ip;
			break;
		case CEE_ADD_OVF_UN:
			binary_arith_op (MINT_ADD_OVF_UN_I4);
			++ip;
			break;
		case CEE_MUL_OVF:
			binary_arith_op (MINT_MUL_OVF_I4);
			++ip;
			break;
		case CEE_MUL_OVF_UN:
			binary_arith_op (MINT_MUL_OVF_UN_I4);
			++ip;
			break;
		case CEE_SUB_OVF:
			binary_arith_op (MINT_SUB_OVF_I4);
			++ip;
			break;
		case CEE_SUB_OVF_UN:
			binary_arith_op (MINT_SUB_OVF_UN_I4);
			++ip;
			break;
		case CEE_ENDFINALLY: {
			g_assert (clause_indexes [in_offset] != -1);
			sp = stack;
			interp_add_ins (MINT_ENDFINALLY);
			last_ins->data [0] = clause_indexes [in_offset];
			link_bblocks = FALSE;
			++ip;
			break;
		}
		case CEE_LEAVE:
		case CEE_LEAVE_S: {
			int target_offset;

			if (*ip == CEE_LEAVE)
				target_offset = 5 + read32 (ip + 1);
			else
				target_offset = 2 + (gint8)ip [1];

			sp = stack;

			for (i = 0; i < header->num_clauses; ++i) {
				MonoExceptionClause *clause = &header->clauses [i];
				if (clause->flags != MONO_EXCEPTION_CLAUSE_FINALLY)
					continue;
				if (MONO_OFFSET_IN_CLAUSE (clause, (ip - header->code)) &&
						(!MONO_OFFSET_IN_CLAUSE (clause, (target_offset + in_offset)))) {
					handle_branch (MINT_CALL_HANDLER_S, MINT_CALL_HANDLER, clause->handler_offset - in_offset);
					// FIXME We need new IR to get rid of _S ugliness
					if (last_ins->opcode == MINT_CALL_HANDLER_S)
						last_ins->data [1] = i;
					else
						last_ins->data [2] = i;
				}
			}

			if (clause_indexes [in_offset] != -1) {
				/* LEAVE instructions in catch clauses need to check for abort exceptions */
				handle_branch (MINT_LEAVE_S_CHECK, MINT_LEAVE_CHECK, target_offset);
			} else {
				handle_branch (MINT_LEAVE_S, MINT_LEAVE, target_offset);
			}

			if (*ip == CEE_LEAVE)
				ip += 5;
			else
				ip += 2;
			link_bblocks = FALSE;
			break;
		}
		case MONO_CUSTOM_PREFIX:
			++ip;
		        switch (*ip) {
				case CEE_MONO_RETHROW:
					CHECK_STACK (1);
					interp_add_ins (MINT_MONO_RETHROW);
					sp--;
					interp_ins_set_sreg (last_ins, sp [0].local);
					sp = stack;
					++ip;
					break;

				case CEE_MONO_LD_DELEGATE_METHOD_PTR:
					--sp;
					ip += 1;
					interp_add_ins (MINT_LD_DELEGATE_METHOD_PTR);
					interp_ins_set_sreg (last_ins, sp [0].local);
					push_simple_type (StackType::I);
					interp_ins_set_dreg (last_ins, sp [-1].local);		
					break;
				case CEE_MONO_CALLI_EXTRA_ARG: {
					int saved_local = sp [-1].local;
					/* Same as CEE_CALLI, except that we drop the extra arg required for llvm specific behaviour */
					sp -= 2;
					StackInfo tos = sp [1];

					// Push back to top of stack and fixup the local offset
					push_types (&tos, 1);
					sp [-1].local = saved_local;
					locals [saved_local].stack_offset = sp [-1].offset;

					if (!interp_transform_call (method, NULL, domain, generic_context, NULL, FALSE, error, FALSE, FALSE, FALSE))
						goto exit;
					break;
				}
				case CEE_MONO_JIT_ICALL_ADDR: {
					const guint32 token = read32 (ip + 1);
					ip += 5;
					const gconstpointer func = mono_find_jit_icall_info ((MonoJitICallId)token)->func;

					interp_add_ins (MINT_MONO_LDPTR);
					push_simple_type (StackType::I);
					interp_ins_set_dreg (last_ins, sp [-1].local);
					last_ins->data [0] = get_data_item_index ((gpointer)func);
					break;
				}
				case CEE_MONO_ICALL: {
					int dreg;
					MonoJitICallId const jit_icall_id = (MonoJitICallId)read32 (ip + 1);
					MonoJitICallInfo const * const info = mono_find_jit_icall_info (jit_icall_id);
					ip += 5;

					CHECK_STACK (info->sig->param_count);
					sp -= info->sig->param_count;
					for (int i = 0; i < info->sig->param_count; i++)
						locals [sp [i].local].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
					if (!MONO_TYPE_IS_VOID (info->sig->ret)) {
						MintType mt = mint_type (info->sig->ret);
						push_simple_type (stack_type_of (mt));
						dreg = sp [-1].local;
						locals [dreg].flags |= INTERP_LOCAL_FLAG_CALL_ARGS;
					} else {
						// Create a new dummy local to serve as the dreg of the call
						// This dreg is only used to resolve the call args offset
						push_simple_type (StackType::I4);
						sp--;
						dreg = sp [0].local;
					}
					if (jit_icall_id == MONO_JIT_ICALL_mono_threads_attach_coop) {
						rtm->needs_thread_attach = 1;
					} else if (jit_icall_id == MONO_JIT_ICALL_mono_threads_detach_coop) {
						g_assert (rtm->needs_thread_attach);
					} else {
						int const icall_op = interp_icall_op_for_sig (info->sig);
						g_assert (icall_op != -1);

						interp_add_ins (icall_op);
						// hash here is overkill
						interp_ins_set_dreg (last_ins, dreg);
						last_ins->data [0] = get_data_item_index ((gpointer)info->func);
					}
					break;
				}
			case CEE_MONO_VTADDR: {
				int size;
				CHECK_STACK (1);
				MonoClass *klass = sp [-1].klass;
				if (method->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE)
					size = mono_class_native_size (klass, NULL);
				else
					size = mono_class_value_size (klass, NULL);

				int local = create_interp_local_explicit (m_class_get_byval_arg (klass), size);
				interp_add_ins (MINT_MOV_VT);
				sp--;
				interp_ins_set_sreg (last_ins, sp [0].local);
				interp_ins_set_dreg (last_ins, local);
				last_ins->data [0] = size;

				interp_add_ins (MINT_LDLOCA_S);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				interp_ins_set_sreg (last_ins, local);
				locals [local].indirects++;

				++ip;
				break;
			}
			case CEE_MONO_LDPTR:
			case CEE_MONO_CLASSCONST:
			case CEE_MONO_METHODCONST:
				token = read32 (ip + 1);
				ip += 5;
				interp_add_ins (MINT_MONO_LDPTR);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				last_ins->data [0] = get_data_item_index (mono_method_get_wrapper_data (method, token));
				break;
			case CEE_MONO_PINVOKE_ADDR_CACHE: {
				token = read32 (ip + 1);
				ip += 5;
				interp_add_ins (MINT_MONO_LDPTR);
				g_assert (method->wrapper_type != MONO_WRAPPER_NONE);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				/* This is a memory slot used by the wrapper */
				gpointer addr = mono_mem_manager_alloc0 (mem_manager, sizeof (gpointer));
				last_ins->data [0] = get_data_item_index (addr);
				break;
			}
			case CEE_MONO_OBJADDR:
				CHECK_STACK (1);
				++ip;
				sp[-1].type = StackType::MP;
				/* do nothing? */
				break;
			case CEE_MONO_NEWOBJ:
				token = read32 (ip + 1);
				ip += 5;
				interp_add_ins (MINT_MONO_NEWOBJ);
				push_simple_type (StackType::O);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				last_ins->data [0] = get_data_item_index (mono_method_get_wrapper_data (method, token));
				break;
			case CEE_MONO_RETOBJ:
				CHECK_STACK (1);
				token = read32 (ip + 1);
				ip += 5;
				interp_add_ins (MINT_MONO_RETOBJ);
				sp--;
				interp_ins_set_sreg (last_ins, sp [0].local);
				klass = (MonoClass *)mono_method_get_wrapper_data (method, token);
				
				/*stackval_from_data (signature->ret, frame->retval, sp->data.vt, signature->pinvoke);*/

				if (sp > stack)
					g_warning ("CEE_MONO_RETOBJ: more values on stack: %d", sp-stack);
				break;
			case CEE_MONO_LDNATIVEOBJ: {
				token = read32 (ip + 1);
				ip += 5;
				klass = (MonoClass *)mono_method_get_wrapper_data (method, token);
				g_assert (m_class_is_valuetype (klass));
				sp--;

				int size = mono_class_native_size (klass, NULL);
				interp_add_ins (MINT_LDOBJ_VT);
				interp_ins_set_sreg (last_ins, sp [0].local);
				push_type_vt (klass, size);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				last_ins->data [0] = size;
				break;
			}
			case CEE_MONO_TLS: {
				gint32 key = read32 (ip + 1);
				ip += 5;
				g_assertf (key == TLS_KEY_SGEN_THREAD_INFO, "%d", key);
				interp_add_ins (MINT_MONO_SGEN_THREAD_INFO);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				break;
			}
			case CEE_MONO_ATOMIC_STORE_I4:
				CHECK_STACK (2);
				interp_add_ins (MINT_MONO_ATOMIC_STORE_I4);
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
				ip += 2;
				break;
			case CEE_MONO_SAVE_LMF:
			case CEE_MONO_RESTORE_LMF:
			case CEE_MONO_NOT_TAKEN:
				++ip;
				break;
			case CEE_MONO_LDPTR_INT_REQ_FLAG:
				interp_add_ins (MINT_MONO_LDPTR);
				push_type (StackType::MP, NULL);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				last_ins->data [0] = get_data_item_index (&mono_thread_interruption_request_flag);
				++ip;
				break;
			case CEE_MONO_MEMORY_BARRIER:
				interp_add_ins (MINT_MONO_MEMORY_BARRIER);
				++ip;
				break;
			case CEE_MONO_LDDOMAIN:
				interp_add_ins (MINT_MONO_LDDOMAIN);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				++ip;
				break;
			case CEE_MONO_SAVE_LAST_ERROR:
				save_last_error = TRUE;
				++ip;
				break;
			case CEE_MONO_GET_SP:
				interp_add_ins (MINT_MONO_GET_SP);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				++ip;
				break;
			default:
				g_error ("transform.c: Unimplemented opcode: 0xF0 %02x at 0x%x\n", *ip, ip-header->code);
			}
			break;
#if 0
		case CEE_PREFIX7:
		case CEE_PREFIX6:
		case CEE_PREFIX5:
		case CEE_PREFIX4:
		case CEE_PREFIX3:
		case CEE_PREFIX2:
		case CEE_PREFIXREF: ves_abort(); break;
#endif
		/*
		 * Note: Exceptions thrown when executing a prefixed opcode need
		 * to take into account the number of prefix bytes (usually the
		 * throw point is just (ip - n_prefix_bytes).
		 */
		case CEE_PREFIX1: 
			++ip;
			switch (*ip) {
			case CEE_ARGLIST:
				load_local (arglist_local);
				++ip;
				break;
			case CEE_CEQ:
				CHECK_STACK (2);
				if (sp [-1].type == StackType::O || sp [-1].type == StackType::MP) {
					interp_add_ins (op_for_stack_type (MINT_CEQ_I4, StackType::I));
				} else {
					if (sp [-1].type == StackType::R4 && sp [-2].type == StackType::R8)
						interp_add_conv (sp - 1, NULL, StackType::R8, MINT_CONV_R8_R4);
					if (sp [-1].type == StackType::R8 && sp [-2].type == StackType::R4)
						interp_add_conv (sp - 2, NULL, StackType::R8, MINT_CONV_R8_R4);
					interp_add_ins (op_for_stack_type (MINT_CEQ_I4, sp [-1].type));
				}
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				++ip;
				break;
			case CEE_CGT:
				CHECK_STACK (2);
				if (sp [-1].type == StackType::O || sp [-1].type == StackType::MP)
					interp_add_ins (op_for_stack_type (MINT_CGT_I4, StackType::I));
				else
					interp_add_ins (op_for_stack_type (MINT_CGT_I4, sp [-1].type));
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				++ip;
				break;
			case CEE_CGT_UN:
				CHECK_STACK (2);
				if (sp [-1].type == StackType::O || sp [-1].type == StackType::MP)
					interp_add_ins (op_for_stack_type (MINT_CGT_UN_I4, StackType::I));
				else
					interp_add_ins (op_for_stack_type (MINT_CGT_UN_I4, sp [-1].type));
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				++ip;
				break;
			case CEE_CLT:
				CHECK_STACK (2);
				if (sp [-1].type == StackType::O || sp [-1].type == StackType::MP)
					interp_add_ins (op_for_stack_type (MINT_CLT_I4, StackType::I));
				else
					interp_add_ins (op_for_stack_type (MINT_CLT_I4, sp [-1].type));
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				++ip;
				break;
			case CEE_CLT_UN:
				CHECK_STACK (2);
				if (sp [-1].type == StackType::O || sp [-1].type == StackType::MP)
					interp_add_ins (op_for_stack_type (MINT_CLT_UN_I4, StackType::I));
				else
					interp_add_ins (op_for_stack_type (MINT_CLT_UN_I4, sp [-1].type));
				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				++ip;
				break;
			case CEE_LDVIRTFTN: /* fallthrough */
			case CEE_LDFTN: {
				MonoMethod *m;
				token = read32 (ip + 1);
				m = interp_get_method (method, token, image, generic_context, error);
				goto_if_nok (error, exit);

				if (!mono_method_can_access_method (method, m))
					interp_generate_mae_throw (method, m);

				/*
				 * Only ldftn takes the wrapper here. ldvirtftn resolves the
				 * override at run time and get_virtual_method () wraps what it
				 * found; handing it the wrapper instead gives it a method that is
				 * not virtual and not in any vtable, so the receiver selects
				 * nothing and the base body is what comes back.
				 */
				if (*ip == CEE_LDFTN && method->wrapper_type == MONO_WRAPPER_NONE &&
				    m->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
					m = mono_marshal_get_synchronized_wrapper (m);

				if (G_UNLIKELY (*ip == CEE_LDFTN &&
						m->wrapper_type == MONO_WRAPPER_NONE &&
						mono_method_has_unmanaged_callers_only_attribute (m))) {

					if (m->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
						interp_generate_not_supported_throw ();
						interp_add_ins (MINT_LDNULL);
						push_simple_type (StackType::MP);
						interp_ins_set_dreg (last_ins, sp [-1].local);
						ip += 5;
						break;
					}

					MonoMethod *ctor_method;

					const unsigned char *next_ip = ip + 5;
					/* check for
					 *    ldftn method_sig
					 *    newobj Delegate::.ctor
					 */
					if (next_ip < end &&
					    *next_ip == CEE_NEWOBJ &&
					    ((ctor_method = interp_get_method (method, read32 (next_ip + 1), image, generic_context, error))) &&
					    is_ok (error) &&
					    m_class_get_parent (ctor_method->klass) == mono_defaults.multicastdelegate_class &&
					    !strcmp (ctor_method->name, ".ctor")) {
						mono_error_set_not_supported (error, "Cannot create delegate from method with UnmanagedCallersOnlyAttribute");
						goto exit;
					}

					MonoClass *delegate_klass = NULL;
					MonoGCHandle target_handle = 0;
					ERROR_DECL (wrapper_error);
					m = mono_marshal_get_managed_wrapper (m, delegate_klass, target_handle, wrapper_error);
					if (!is_ok (wrapper_error)) {
						/* Generate a call that will throw an exception if the
						 * UnmanagedCallersOnly attribute is used incorrectly */
						interp_generate_ipe_throw_with_msg (wrapper_error);
						mono_interp_error_cleanup (wrapper_error);
						interp_add_ins (MINT_LDNULL);
						push_simple_type (StackType::MP);
						interp_ins_set_dreg (last_ins, sp [-1].local);
					} else {
						/* push a pointer to a trampoline that calls m */
						gpointer entry = mini_get_interp_callbacks ()->create_method_pointer (m, TRUE, error);
#if SIZEOF_VOID_P == 8
						interp_add_ins (MINT_LDC_I8);
						WRITE64_INS (last_ins, 0, &entry);
#else
						interp_add_ins (MINT_LDC_I4);
						WRITE32_INS (last_ins, 0, &entry);
#endif
						push_simple_type (StackType::MP);
						interp_ins_set_dreg (last_ins, sp [-1].local);
					}
					ip += 5;
					break;
				}
			
				/*
				 * A method pointer is the method's entry point in both engines, so
				 * that a delegate built from one - or a calli through it - means the
				 * same thing wherever the frame that produced it ran. Both opcodes
				 * carry the InterpMethod and ask for its entry when they run: the
				 * entry is a trampoline, and creating one for every ldftn site the
				 * program never reaches would be worse than the load.
				 */
				int index = get_data_item_index (mono_interp_get_imethod (domain, m, error));
				goto_if_nok (error, exit);
				if (*ip == CEE_LDVIRTFTN) {
					CHECK_STACK (1);
					--sp;
					interp_add_ins (MINT_LDVIRTFTN);
					interp_ins_set_sreg (last_ins, sp [0].local);
					last_ins->data [0] = index;
				} else {
					interp_add_ins (MINT_LDFTN);
					last_ins->data [0] = index;
				}
				push_simple_type (StackType::F);
				interp_ins_set_dreg (last_ins, sp [-1].local);

				ip += 5;
				break;
			}
			case CEE_LDARG: {
				int arg_n = read16 (ip + 1);
				if (!inlining)
					load_arg (arg_n);
				else
					load_local (arg_locals [arg_n]);
				ip += 3;
				break;
			}
			case CEE_LDARGA: {
				int n = read16 (ip + 1);

				if (!inlining) {
					interp_add_ins (MINT_LDLOCA_S);
					interp_ins_set_sreg (last_ins, n);
					locals [n].indirects++;
				} else {
					int loc_n = arg_locals [n];
					interp_add_ins (MINT_LDLOCA_S);
					interp_ins_set_sreg (last_ins, loc_n);
					locals [loc_n].indirects++;
				}
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				ip += 3;
				break;
			}
			case CEE_STARG: {
				int arg_n = read16 (ip + 1);
				if (!inlining)
					store_arg (arg_n);
				else
					store_local (arg_locals [arg_n]);
				ip += 3;
				break;
			}
			case CEE_LDLOC: {
				int loc_n = read16 (ip + 1);
				if (!inlining)
					load_local (num_args + loc_n);
				else
					load_local (local_locals [loc_n]);
				ip += 3;
				break;
			}
			case CEE_LDLOCA: {
				int loc_n = read16 (ip + 1);
				interp_add_ins (MINT_LDLOCA_S);
				if (!inlining)
					loc_n += num_args;
				else
					loc_n = local_locals [loc_n];
				interp_ins_set_sreg (last_ins, loc_n);
				locals [loc_n].indirects++;
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				ip += 3;
				break;
			}
			case CEE_STLOC: {
				int loc_n = read16 (ip + 1);
				if (!inlining)
					store_local (num_args + loc_n);
				else
					store_local (local_locals [loc_n]);
				ip += 3;
				break;
			}
			case CEE_LOCALLOC:
				INLINE_FAILURE;
				CHECK_STACK (1);
#if SIZEOF_VOID_P == 8
				if (sp [-1].type == StackType::I8)
					interp_add_conv (sp - 1, NULL, StackType::I4, MINT_CONV_I4_I8);
#endif				
				interp_add_ins (MINT_LOCALLOC);
				if (sp != stack + 1)
					g_warning("CEE_LOCALLOC: stack not empty");
				sp--;
				interp_ins_set_sreg (last_ins, sp [0].local);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				has_localloc = TRUE;
				++ip;
				break;
#if 0
			case CEE_UNUSED57: ves_abort(); break;
#endif
			case CEE_ENDFILTER:
				interp_add_ins (MINT_ENDFILTER);
				interp_ins_set_sreg (last_ins, sp [-1].local);
				++ip;
				link_bblocks = FALSE;
				break;
			case CEE_UNALIGNED_:
				ip += 2;
				break;
			case CEE_VOLATILE_:
				++ip;
				volatile_ = TRUE;
				break;
			case CEE_TAIL_:
				++ip;
				tailcall = TRUE;
				// TODO: This should raise a method_tail_call profiler event.
				break;
			case CEE_INITOBJ:
				CHECK_STACK (1);
				token = read32 (ip + 1);
				klass = mini_get_class (method, token, generic_context);
				CHECK_TYPELOAD (klass);
				if (m_class_is_valuetype (klass)) {
					--sp;
					interp_add_ins (MINT_INITOBJ);
					interp_ins_set_sreg (last_ins, sp [0].local);
					i32 = mono_class_value_size (klass, NULL);
					g_assert (i32 < G_MAXUINT16);
					last_ins->data [0] = i32;
				} else {
					interp_add_ins (MINT_LDNULL);
					push_type (StackType::O, NULL);
					interp_ins_set_dreg (last_ins, sp [-1].local);

					interp_add_ins (MINT_STIND_REF);
					sp -= 2;
					interp_ins_set_sregs2 (last_ins, sp [0].local, sp [1].local);
				}
				ip += 5;
				break;
			case CEE_CPBLK:
				CHECK_STACK (3);
				/* FIX? convert length to I8? */
				if (volatile_)
					interp_add_ins (MINT_MONO_MEMORY_BARRIER);
				interp_add_ins (MINT_CPBLK);
				sp -= 3;
				interp_ins_set_sregs3 (last_ins, sp [0].local, sp [1].local, sp [2].local);
				BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_SEQ);
				++ip;
				break;
			case CEE_READONLY_:
				readonly = TRUE;
				ip += 1;
				break;
			case CEE_CONSTRAINED_:
				token = read32 (ip + 1);
				constrained_class = mini_get_class (method, token, generic_context);
				CHECK_TYPELOAD (constrained_class);
				ip += 5;
				break;
			case CEE_INITBLK:
				CHECK_STACK (3);
				BARRIER_IF_VOLATILE (MONO_MEMORY_BARRIER_REL);
				interp_add_ins (MINT_INITBLK);
				sp -= 3;
				interp_ins_set_sregs3 (last_ins, sp [0].local, sp [1].local, sp [2].local);
				ip += 1;
				break;
			case CEE_NO_:
				/* FIXME: implement */
				ip += 2;
				break;
			case CEE_RETHROW: {
				int clause_index = clause_indexes [in_offset];
				g_assert (clause_index != -1);
				interp_add_ins (MINT_RETHROW);
				last_ins->data [0] = rtm->clause_data_offsets [clause_index];
				sp = stack;
				link_bblocks = FALSE;
				++ip;
				break;
			}
			case CEE_SIZEOF: {
				gint32 size;
				token = read32 (ip + 1);
				ip += 5;
				if (mono_metadata_token_table (token) == MONO_TABLE_TYPESPEC && !image_is_dynamic (m_class_get_image (method->klass)) && !generic_context) {
					int align;
					MonoType *type = mono_type_create_from_typespec_checked (image, token, error);
					goto_if_nok (error, exit);
					size = mono_type_size (type, &align);
				} else {
					int align;
					MonoClass *szclass = mini_get_class (method, token, generic_context);
					CHECK_TYPELOAD (szclass);
#if 0
					if (!szclass->valuetype)
						THROW_EX (mono_exception_from_name (mono_defaults.corlib, "System", "InvalidProgramException"), ip - 5);
#endif
					size = mono_type_size (m_class_get_byval_arg (szclass), &align);
				} 
				interp_add_ins (MINT_LDC_I4);
				WRITE32_INS (last_ins, 0, &size);
				push_simple_type (StackType::I4);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				break;
			}
			case CEE_REFANYTYPE:
				interp_add_ins (MINT_REFANYTYPE);
				sp--;
				interp_ins_set_sreg (last_ins, sp [0].local);
				push_simple_type (StackType::I);
				interp_ins_set_dreg (last_ins, sp [-1].local);
				++ip;
				break;
			default:
				g_error ("transform.c: Unimplemented opcode: 0xFE %02x (%s) at 0x%x\n", *ip, mono_opcode_name (256 + *ip), ip-header->code);
			}
			break;
		default:
			g_error ("transform.c: Unimplemented opcode: %02x at 0x%x\n", *ip, ip-header->code);
		}
		// No IR instructions were added as part of a bb_start IL instruction. Add a MINT_NOP
		// so we always have an instruction associated with a bb_start. This is simple and avoids
		// any complications associated with il_offset tracking.
		if (!cbb->last_ins)
			interp_add_ins (MINT_NOP);
	}

	g_assert (ip == end);

	if (inlining) {
		// When inlining, all return points branch to this bblock. Code generation inside the caller
		// method continues in this bblock. exit_bb is not necessarily an out bb for cbb. We need to
		// restore stack state so future codegen can work.
		cbb->next_bb = exit_bb;
		cbb = exit_bb;
		if (exit_bb->stack_height >= 0) {
			if (exit_bb->stack_height > 0)
				memcpy (stack, exit_bb->stack_state, exit_bb->stack_height * sizeof(stack [0]));
			sp = stack + exit_bb->stack_height;
		}
	}

	if (sym_seq_points) {
		for (InterpBasicBlock *bb : blocks_from (entry_bb->next_bb)) {
			if (bb->first_ins && bb->in_count > 1 && bb->first_ins->opcode == MINT_SDB_SEQ_POINT)
				interp_insert_ins_bb (bb, NULL, MINT_SDB_INTR_LOC);
		}
	}

exit_ret:
	g_free (arg_locals);
	g_free (local_locals);
	mono_basic_block_free (original_bb);

	return ret;
exit:
	ret = FALSE;
	goto exit_ret;
}

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

int
TransformData::get_native_offset (int il_offset)
{
	// We can't access offset_to_bb for header->code_size IL offset. Also, offset_to_bb
	// is not set for dead bblocks at method end.
	if (il_offset < header->code_size && offset_to_bb [il_offset]) {
		InterpBasicBlock *bb = offset_to_bb [il_offset];
		g_assert (!bb->dead);
		return bb->native_offset;
	} else {
		return new_code_end - new_code;
	}
}

TransformData::TransformData (MonoMethod *method, MonoMethodHeader *header, InterpMethod *rtm)
	: method (method), header (header), rtm (rtm), has_localloc (false)
{
	code_size = header->code_size;
	max_code_size = code_size;

	in_offsets = g_new0 (int, header->code_size + 1);
	clause_indexes = g_new (int, header->code_size);
	mem_manager = m_method_get_mem_manager (rtm->domain, method);
	gen_sdb_seq_points = mini_debug_options.gen_sdb_seq_points;
	verbose_level = mono_interp_traceopt;
	prof_coverage = mono_profiler_coverage_instrumentation_enabled (method);

	if (prof_coverage)
		coverage_info = mono_profiler_coverage_alloc (rtm->domain, method, header->code_size);

	stack = g_new0 (StackInfo, header->max_stack + 1);
	stack_capacity = header->max_stack + 1;
	sp = stack;
}

TransformData::~TransformData ()
{
	g_free (in_offsets);
	g_free (clause_indexes);
	g_free (stack);
}

static void
generate (MonoMethod *method, MonoMethodHeader *header, InterpMethod *rtm, MonoGenericContext *generic_context, MonoError *error)
{
	int i;
	TransformData transform_data (method, header, rtm);
	TransformData *td = &transform_data;
	static gboolean verbose_method_inited;
	static char* verbose_method_name;

	if (!verbose_method_inited) {
		verbose_method_name = g_getenv ("MONO_VERBOSE_METHOD");
		verbose_method_inited = TRUE;
	}

	rtm->data_items = NULL;

	td->interp_method_compute_offsets (rtm, mono_method_signature_internal (method), header, error);
	return_if_nok (error);

	if (verbose_method_name) {
		const char *name = verbose_method_name;

		if ((strchr (name, '.') > name) || strchr (name, ':')) {
			MonoMethodDesc *desc;

			desc = mono_method_desc_new (name, TRUE);
			if (mono_method_desc_full_match (desc, method)) {
				td->verbose_level = 4;
			}
			mono_method_desc_free (desc);
		} else {
			if (strcmp (method->name, name) == 0)
				td->verbose_level = 4;
		}
	}

	td->generate_code (method, header, generic_context, error);
	return_if_nok (error);

	g_assert (td->inline_depth == 0);

	if (td->has_localloc)
		td->interp_fix_localloc_ret ();

	td->interp_optimize_code ();

	td->generate_compacted_code ();

	if (td->total_locals_size >= G_MAXUINT16) {
		char *name = mono_method_get_full_name (method);
		char *msg = g_strdup_printf ("Unable to run method '%s': locals size too big.", name);
		g_free (name);
		mono_error_set_generic_error (error, "System", "InvalidProgramException", "%s", msg);
		g_free (msg);
		return;
	}

	if (td->verbose_level) {
		g_print ("Runtime method: %s %p\n", mono_method_full_name (method, TRUE), rtm);
		g_print ("Locals size %d, stack size: %d\n", td->total_locals_size, td->max_stack_size);
		g_print ("Calculated stack height: %d, stated height: %d\n", td->max_stack_height, header->max_stack);
		dump_interp_code (td->new_code, td->new_code_end);
	}

	/* Check if we use excessive stack space */
	if (td->max_stack_height > header->max_stack * 3 && header->max_stack > 16)
		g_warning ("Excessive stack space usage for method %s, %d/%d", method->name, td->max_stack_height, header->max_stack);

	int code_len_u8, code_len_u16;
	code_len_u8 = (guint8 *) td->new_code_end - (guint8 *) td->new_code;
	code_len_u16 = td->new_code_end - td->new_code;

	rtm->clauses = (MonoExceptionClause*)mono_mem_manager_alloc0 (td->mem_manager, header->num_clauses * sizeof (MonoExceptionClause));
	memcpy (rtm->clauses, header->clauses, header->num_clauses * sizeof(MonoExceptionClause));
	rtm->code = (gushort*)td->new_code;
	rtm->init_locals = header->init_locals;
	rtm->num_clauses = header->num_clauses;
	for (i = 0; i < header->num_clauses; i++) {
		MonoExceptionClause *c = rtm->clauses + i;
		int end_off = c->try_offset + c->try_len;
		c->try_offset = td->get_native_offset (c->try_offset);
		c->try_len = td->get_native_offset (end_off) - c->try_offset;
		g_assert ((c->try_offset + c->try_len) <= code_len_u16);
		end_off = c->handler_offset + c->handler_len;
		c->handler_offset = td->get_native_offset (c->handler_offset);
		c->handler_len = td->get_native_offset (end_off) - c->handler_offset;
		g_assert (c->handler_len >= 0 && (c->handler_offset + c->handler_len) <= code_len_u16);
		if (c->flags & MONO_EXCEPTION_CLAUSE_FILTER)
			c->data.filter_offset = td->get_native_offset (c->data.filter_offset);
	}
	rtm->stack_size = td->max_stack_size;
	// FIXME revisit whether we actually need this
	rtm->stack_size += 2 * MINT_STACK_SLOT_SIZE; /* + 1 for returns of called functions  + 1 for 0-ing in trace*/
	rtm->total_locals_size = ALIGN_TO (td->total_locals_size, MINT_VT_ALIGNMENT);
	rtm->alloca_size = ALIGN_TO (rtm->total_locals_size + rtm->stack_size, 8);
	size_t data_items_size = td->data_items.size () * sizeof (td->data_items[0]);
	rtm->data_items = (gpointer *) mono_mem_manager_alloc0 (td->mem_manager, data_items_size);
	memcpy (rtm->data_items, td->data_items.data (), data_items_size);

	td->interp_save_line_numbers (rtm, td->line_numbers);

	/* Save debug info */
	td->interp_save_debug_info (rtm, header, td->line_numbers);

	/* Create a MonoJitInfo for the interpreted method by creating the interpreter IR as the native code. */
	int jinfo_len;
	jinfo_len = mono_jit_info_size ((MonoJitInfoFlags)0, header->num_clauses, 0);
	MonoJitInfo *jinfo;
	jinfo = (MonoJitInfo *)mono_mem_manager_alloc0 (td->mem_manager, jinfo_len);
	jinfo->is_interp = 1;
	rtm->jinfo = jinfo;
	mono_jit_info_init (jinfo, method, (guint8*)rtm->code, code_len_u8, (MonoJitInfoFlags)0, header->num_clauses, 0);
	for (i = 0; i < jinfo->num_clauses; ++i) {
		MonoJitExceptionInfo *ei = &jinfo->clauses [i];
		MonoExceptionClause *c = rtm->clauses + i;

		ei->flags = c->flags;
		ei->try_start = (guint8*)(rtm->code + c->try_offset);
		ei->try_end = (guint8*)(rtm->code + c->try_offset + c->try_len);
		ei->handler_start = (guint8*)(rtm->code + c->handler_offset);
		/* Into frame_locals (), not a machine frame, so exvar_base_reg is unused here. */
		ei->exvar_offset = rtm->clause_data_offsets [i];
		if (ei->flags == MONO_EXCEPTION_CLAUSE_FILTER) {
			ei->data.filter = (guint8*)(rtm->code + c->data.filter_offset);
		} else if (ei->flags == MONO_EXCEPTION_CLAUSE_FINALLY) {
			ei->data.handler_end = (guint8*)(rtm->code + c->handler_offset + c->handler_len);
		} else {
			ei->data.catch_class = c->data.catch_class;
		}
	}

	td->save_seq_points (jinfo);
}


static mono_mutex_t calc_section;

} // namespace mono::interp

/* Outside the namespace, because interp-internals.hpp and transform.hpp
 * declare them there. */

using namespace mono::interp;

/*
 * Whether do_jit_call () can marshal a call to METHOD at all. These are limits
 * of that marshalling - what the gsharedvt_out wrapper can be built for and what
 * the interpreter's stack can be handed across - rather than a policy about when
 * calling natively is worth it. A method that fails here stays interpreted
 * however thoroughly it has been compiled.
 */
gboolean
mono_interp_jit_call_marshallable (MonoMethod *method, MonoMethodSignature *sig)
{
	if (sig->param_count > 6)
		return FALSE;
	/* The callee's own signature says nothing about the arguments a vararg
	 * call site actually pushed, so the wrapper cannot be built for one. */
	if (sig->call_convention == MONO_CALL_VARARG)
		return FALSE;
	if (sig->pinvoke)
		return FALSE;
	if (method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)
		return FALSE;
	if (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL)
		return FALSE;
	if (method->is_inflated)
		return FALSE;
	if (method->string_ctor)
		return FALSE;
	if (method->wrapper_type != MONO_WRAPPER_NONE)
		return FALSE;

	return TRUE;
}

gboolean
mono_interp_jit_call_supported (MonoMethod *method, MonoMethodSignature *sig)
{
	GSList *l;

	if (!mono_interp_jit_call_marshallable (method, sig))
		return FALSE;

	if (mono_aot_only && m_class_get_image (method->klass)->aot_module && !(method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)) {
		ERROR_DECL (error);
		gpointer addr = mono_jit_compile_method_jit_only (method, error);
		if (addr && is_ok (error))
			return TRUE;
	}

	for (l = mono_interp_jit_classes; l; l = l->next) {
		const char *class_name = (const char*)l->data;
		// FIXME: Namespaces
		if (!strcmp (m_class_get_name (method->klass), class_name))
			return TRUE;
	}

	//return TRUE;
	return FALSE;
}

void
mono_interp_dis_mintop (const guint16 *ip, const guint16 *start)
{
	int opcode = *ip;
	int ins_offset = ip - start;

	g_print ("IR_%04x: %-14s", ins_offset, opname (opcode));
	ip++;

        if (num_dregs (opcode) == MINT_CALL_ARGS)
                g_print (" [call_args %d <-", *ip++);
        else if (num_dregs (opcode) > 0)
                g_print (" [%d <-", *ip++);
        else
                g_print (" [nil <-");

        if (num_sregs (opcode) > 0) {
                for (int i = 0; i < num_sregs (opcode); i++)
                        g_print (" %d", *ip++);
                g_print ("],");
        } else {
                g_print (" nil],");
        }
	char *ins = dump_interp_ins_data (NULL, ins_offset, ip, opcode);
	g_print ("%s\n", ins);
	g_free (ins);
}

/* For debug use */
void
mono_interp_print_code (InterpMethod *imethod)
{
	MonoJitInfo *jinfo = imethod->jinfo;
	const guint8 *start;

	if (!jinfo)
		return;

	char *name = mono_method_full_name (imethod->method, 1);
	g_print ("Method : %s\n", name);
	g_free (name);

	start = (guint8*) jinfo->code_start;
	dump_interp_code ((const guint16*)start, (const guint16*)(start + jinfo->code_size));
}

/* For debug use */
void
mono_interp_print_td_code (TransformData *td)
{
	char *name = mono_method_full_name (td->method, TRUE);
	g_print ("IR for \"%s\"\n", name);
	g_free (name);

	for (InterpBasicBlock *bb : blocks_from (td->entry_bb))
		for (InterpInst *ins : *bb)
			dump_interp_inst (ins);
}

void
mono_test_interp_method_compute_offsets (TransformData *td, InterpMethod *imethod, MonoMethodSignature *signature, MonoMethodHeader *header)
{
	ERROR_DECL (error);
	td->interp_method_compute_offsets (imethod, signature, header, error);
}

void
mono_test_interp_cprop (TransformData *td)
{
	td->interp_cprop ();
}

gboolean
mono_test_interp_generate_code (TransformData *td, MonoMethod *method, MonoMethodHeader *header, MonoGenericContext *generic_context, MonoError *error)
{
	return td->generate_code (method, header, generic_context, error);
}

void 
mono_interp_transform_init (void)
{
	mono_os_mutex_init_recursive(&calc_section);

}

void
mono_interp_transform_method (InterpMethod *imethod, ThreadContext *context, MonoError *error)
{
	MonoMethod *method = imethod->method;
	MonoMethodHeader *header = NULL;
	MonoMethodSignature *signature = mono_method_signature_internal (method);
	MonoVTable *method_class_vt;
	MonoGenericContext *generic_context = NULL;
	MonoDomain *domain = imethod->domain;
	InterpMethod tmp_imethod;
	InterpMethod *real_imethod;

	error_init (error);

#ifdef ENABLE_METADATA_UPDATE
	mono_metadata_update_thread_expose_published ();
#endif

	if (mono_class_is_open_constructed_type (m_class_get_byval_arg (method->klass))) {
		mono_error_set_invalid_operation (error, "%s", "Could not execute the method because the containing type is not fully instantiated.");
		return;
	}

	/*
	 * Here rather than where the backend is asked for a method, because the
	 * interpreter reaches a callee without asking. Transform runs once per
	 * method, so a body gets its verdict before its first instruction either
	 * way, and before the class initializer below runs on its behalf.
	 */
	if (!mono_llvm_jit_verify_method (method, error))
		return;

	// g_printerr ("TRANSFORM(0x%016lx): begin %s::%s\n", mono_thread_current (), method->klass->name, method->name);
	method_class_vt = mono_class_vtable_checked (domain, imethod->method->klass, error);
	return_if_nok (error);

	if (!method_class_vt->initialized) {
		mono_runtime_class_init_full (method_class_vt, error);
		return_if_nok (error);
	}

	MONO_PROFILER_RAISE (jit_begin, (method));

	if (mono_method_signature_internal (method)->is_inflated)
		generic_context = mono_method_get_context (method);
	else {
		MonoGenericContainer *generic_container = mono_method_get_generic_container (method);
		if (generic_container)
			generic_context = &generic_container->context;
	}

	if (method->iflags & (METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL | METHOD_IMPL_ATTRIBUTE_RUNTIME)) {
		MonoMethod *nm = NULL;
		if (imethod->transformed) {
			MONO_PROFILER_RAISE (jit_done, (method, imethod->jinfo));
			return;
		}

		/* assumes all internal calls with an array this are built in... */
		if (method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL && (! mono_method_signature_internal (method)->hasthis || m_class_get_rank (method->klass) == 0)) {
			nm = mono_marshal_get_native_wrapper (method, FALSE, FALSE);
			signature = mono_method_signature_internal (nm);
		} else {
			const char *name = method->name;
			if (m_class_get_parent (method->klass) == mono_defaults.multicastdelegate_class) {
				if (*name == '.' && (strcmp (name, ".ctor") == 0)) {
					MonoJitICallInfo *mi = &mono_get_jit_icall_info ()->ves_icall_mono_delegate_ctor;
					nm = mono_marshal_get_icall_wrapper (mi, TRUE);
				} else if (*name == 'I' && (strcmp (name, "Invoke") == 0)) {
					/*
					 * Usually handled during transformation of the caller, but
					 * when the caller is handled by another execution engine
					 * (for example fullAOT) we need to handle it here. That's
					 * known to be wrong in cases where the reference to
					 * `MonoDelegate` would be needed (FIXME).
					 */
					nm = mono_marshal_get_delegate_invoke (method, NULL);
				} else if (*name == 'B' && (strcmp (name, "BeginInvoke") == 0)) {
					nm = mono_marshal_get_delegate_begin_invoke (method);
				} else if (*name == 'E' && (strcmp (name, "EndInvoke") == 0)) {
					nm = mono_marshal_get_delegate_end_invoke (method);
				}
			}
			if (nm == NULL)
				g_assert_not_reached ();
		}
		if (nm == NULL) {
			mono_os_mutex_lock (&calc_section);
			imethod->stack_size = sizeof (stackval); /* for tracing */
			imethod->alloca_size = imethod->stack_size;
			mono_memory_barrier ();
			imethod->transformed = TRUE;
			mono_interp_stats.methods_transformed++;
			mono_os_mutex_unlock (&calc_section);
			MONO_PROFILER_RAISE (jit_done, (method, NULL));
			return;
		}
		method = nm;
		header = interp_method_get_header (nm, error);
		return_if_nok (error);
	}

	if (!header) {
		header = mono_method_get_header_checked (method, error);
		return_if_nok (error);
	}

	g_assert ((signature->param_count + signature->hasthis) < 1000);
	// g_printerr ("TRANSFORM(0x%016lx): end %s::%s\n", mono_thread_current (), method->klass->name, method->name);

	/* Make modifications to a copy of imethod, copy them back inside the lock */
	real_imethod = imethod;
	memcpy (&tmp_imethod, imethod, sizeof (InterpMethod));
	imethod = &tmp_imethod;

	MONO_TIME_TRACK (mono_interp_stats.transform_time, generate (method, header, imethod, generic_context, error));

	mono_metadata_free_mh (header);

	return_if_nok (error);

	/* Copy changes back */
	imethod = real_imethod;
	/*
	 * A thread that reads transformed runs the bytecode immediately, so any
	 * breakpoint the debugger already asked for must be in that bytecode before
	 * the flag goes up. The jit_done event below is too late: the method is
	 * reachable by then, and a thread that enters in between runs the method to
	 * the end without stopping.
	 *
	 * The loader lock is outside calc_section because installing a breakpoint
	 * takes it. It is recursive, so a caller that holds it already is safe.
	 *
	 * Only a body that carries its own sequence points is offered. Without them
	 * the installer looks for the method's table, which takes the domain lock -
	 * and the domain lock is outer to calc_section, so asking for it here
	 * inverts the order. That table also describes some other body than this
	 * one, so there is nothing here to install from.
	 */
	mono_loader_lock ();
	mono_os_mutex_lock (&calc_section);
	if (!imethod->transformed) {
		// Ignore the first two fields which are unchanged. next_jit_code_hash shouldn't
		// be modified because it is racy with internal hash table insert.
		const int start_offset = 2 * sizeof (gpointer);
		memcpy ((char*)imethod + start_offset, (char*)&tmp_imethod + start_offset, sizeof (InterpMethod) - start_offset);
		if (imethod->jinfo->seq_points)
			mini_install_pending_breakpoints (domain, method, imethod->jinfo);
		mono_memory_barrier ();
		imethod->transformed = TRUE;
		mono_interp_stats.methods_transformed++;
		mono_atomic_fetch_add_i32 (&mono_jit_stats.methods_with_interp, 1);

	}
	mono_os_mutex_unlock (&calc_section);
	mono_loader_unlock ();

	mono_domain_lock (domain);
	if (mono_stats_method_desc && mono_method_desc_full_match (mono_stats_method_desc, imethod->method)) {
		g_printf ("Printing runtime stats at method: %s\n", mono_method_get_full_name (imethod->method));
		mono_runtime_print_stats ();
	}
	/*
	 * A method's entry here is the list of tables its bodies published, and the
	 * domain frees it as a list. So this publishes a node, not the table. The
	 * same method appends its compiled body's table to this list when it is
	 * promoted out of the interpreter.
	 */
	if (imethod->jinfo->seq_points && !g_hash_table_lookup (domain_jit_info (domain)->seq_points, imethod->method))
		g_hash_table_insert (domain_jit_info (domain)->seq_points, imethod->method,
				     g_slist_append (NULL, imethod->jinfo->seq_points));
	mono_domain_unlock (domain);

	// FIXME: Add a different callback ?
	MONO_PROFILER_RAISE (jit_done, (method, imethod->jinfo));
}
