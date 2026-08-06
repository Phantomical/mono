/**
 * \file
 * The new Mono code generator.
 *
 * Authors:
 *   Paolo Molaro (lupus@ximian.com)
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * Copyright 2002-2003 Ximian, Inc.
 * Copyright 2003-2010 Novell, Inc.
 * Copyright 2011 Xamarin, Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include <config.h>
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#if defined (__linux__) && defined (ENABLE_LLVM)
#include <sys/syscall.h>
#include <linux/membarrier.h>
#endif
#include <math.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <mono/utils/memcheck.h>

#include <mono/metadata/assembly.h>
#include <mono/metadata/loader.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/class.h>
#include <mono/metadata/object.h>
#include <mono/metadata/tokentype.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/profiler-private.h>
#include <mono/metadata/mono-config.h>
#include <mono/metadata/environment.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/gc-internals.h>
#include <mono/metadata/threads-types.h>
#include <mono/metadata/verify.h>
#include <mono/metadata/verify-internals.h>
#include <mono/metadata/mempool-internals.h>
#include <mono/metadata/attach.h>
#include <mono/metadata/runtime.h>
#include <mono/metadata/attrdefs.h>
#include <mono/utils/mono-math.h>
#include <mono/utils/mono-compiler.h>
#include <mono/utils/mono-counters.h>
#include <mono/utils/mono-error-internals.h>
#include <mono/utils/mono-logger-internals.h>
#include <mono/utils/mono-mmap.h>
#include <mono/utils/mono-path.h>
#include <mono/utils/mono-tls.h>
#include <mono/utils/mono-hwcap.h>
#include <mono/utils/dtrace.h>
#include <mono/utils/mono-threads.h>
#include <mono/utils/mono-threads-coop.h>
#include <mono/utils/unlocked.h>
#include <mono/utils/mono-time.h>

#include "mini.h"
#include "seq-points.h"
#include "tasklets.h"
#include <string.h>
#include <ctype.h>
#include "trace.h"
#include "ir-emit.h"

#include "jit-icalls.h"

#include "mini-gc.h"
#include "debugger-agent.h"
#include "llvm-runtime.h"
#include "mini-llvm.h"
#include "lldb.h"
#include "aot-runtime.h"
#include "mini-runtime.h"

#include "mixed_callstack_plugin.h"

MonoCallSpec *mono_jit_trace_calls;
MonoMethodDesc *mono_inject_async_exc_method;
int mono_inject_async_exc_pos;
MonoMethodDesc *mono_break_at_bb_method;
int mono_break_at_bb_bb_num;
gboolean mono_do_x86_stack_align = TRUE;

#define mono_jit_lock() mono_os_mutex_lock (&jit_mutex)
#define mono_jit_unlock() mono_os_mutex_unlock (&jit_mutex)
static mono_mutex_t jit_mutex;

#ifndef DISABLE_JIT

typedef struct {
	MonoExceptionClause *clause;
	MonoBasicBlock *basic_block;
	int start_offset;
} TryBlockHole;

/**
 * mono_emit_unwind_op:
 *
 *   Add an unwind op with the given parameters for the list of unwind ops stored in
 * cfg->unwind_ops.
 */
void
mono_emit_unwind_op (MonoCompile *cfg, int when, int tag, int reg, int val)
{
	MonoUnwindOp *op = (MonoUnwindOp *)mono_mempool_alloc0 (cfg->mempool, sizeof (MonoUnwindOp));

	op->op = tag;
	op->reg = reg;
	op->val = val;
	op->when = when;
	
	cfg->unwind_ops = g_slist_append_mempool (cfg->mempool, cfg->unwind_ops, op);
	if (cfg->verbose_level > 1) {
		switch (tag) {
		case DW_CFA_def_cfa:
			printf ("CFA: [%x] def_cfa: %s+0x%x\n", when, mono_arch_regname (reg), val);
			break;
		case DW_CFA_def_cfa_register:
			printf ("CFA: [%x] def_cfa_reg: %s\n", when, mono_arch_regname (reg));
			break;
		case DW_CFA_def_cfa_offset:
			printf ("CFA: [%x] def_cfa_offset: 0x%x\n", when, val);
			break;
		case DW_CFA_offset:
			printf ("CFA: [%x] offset: %s at cfa-0x%x\n", when, mono_arch_regname (reg), -val);
			break;
		}
	}
}

/**
 * mono_unlink_bblock:
 *
 *   Unlink two basic blocks.
 */
void
mono_unlink_bblock (MonoCompile *cfg, MonoBasicBlock *from, MonoBasicBlock* to)
{
	int i, pos;
	gboolean found;

	found = FALSE;
	for (i = 0; i < from->out_count; ++i) {
		if (to == from->out_bb [i]) {
			found = TRUE;
			break;
		}
	}
	if (found) {
		pos = 0;
		for (i = 0; i < from->out_count; ++i) {
			if (from->out_bb [i] != to)
				from->out_bb [pos ++] = from->out_bb [i];
		}
		g_assert (pos == from->out_count - 1);
		from->out_count--;
	}

	found = FALSE;
	for (i = 0; i < to->in_count; ++i) {
		if (from == to->in_bb [i]) {
			found = TRUE;
			break;
		}
	}
	if (found) {
		pos = 0;
		for (i = 0; i < to->in_count; ++i) {
			if (to->in_bb [i] != from)
				to->in_bb [pos ++] = to->in_bb [i];
		}
		g_assert (pos == to->in_count - 1);
		to->in_count--;
	}
}

/*
 * mono_bblocks_linked:
 *
 *   Return whenever BB1 and BB2 are linked in the CFG.
 */
gboolean
mono_bblocks_linked (MonoBasicBlock *bb1, MonoBasicBlock *bb2)
{
	int i;

	for (i = 0; i < bb1->out_count; ++i) {
		if (bb1->out_bb [i] == bb2)
			return TRUE;
	}

	return FALSE;
}

static int
mono_find_block_region_notry (MonoCompile *cfg, int offset)
{
	MonoMethodHeader *header = cfg->header;
	MonoExceptionClause *clause;
	int i;

	for (i = 0; i < header->num_clauses; ++i) {
		clause = &header->clauses [i];
		if ((clause->flags == MONO_EXCEPTION_CLAUSE_FILTER) && (offset >= clause->data.filter_offset) &&
		    (offset < (clause->handler_offset)))
			return ((i + 1) << 8) | MONO_REGION_FILTER | clause->flags;
			   
		if (MONO_OFFSET_IN_HANDLER (clause, offset)) {
			if (clause->flags == MONO_EXCEPTION_CLAUSE_FINALLY)
				return ((i + 1) << 8) | MONO_REGION_FINALLY | clause->flags;
			else if (clause->flags == MONO_EXCEPTION_CLAUSE_FAULT)
				return ((i + 1) << 8) | MONO_REGION_FAULT | clause->flags;
			else
				return ((i + 1) << 8) | MONO_REGION_CATCH | clause->flags;
		}
	}

	return -1;
}

/*
 * mono_get_block_region_notry:
 *
 *   Return the region corresponding to REGION, ignoring try clauses nested inside
 * finally clauses.
 */
int
mono_get_block_region_notry (MonoCompile *cfg, int region)
{
	if ((region & (0xf << 4)) == MONO_REGION_TRY) {
		MonoMethodHeader *header = cfg->header;
		
		/*
		 * This can happen if a try clause is nested inside a finally clause.
		 */
		int clause_index = (region >> 8) - 1;
		g_assert (clause_index >= 0 && clause_index < header->num_clauses);
		
		region = mono_find_block_region_notry (cfg, header->clauses [clause_index].try_offset);
	}

	return region;
}

MonoInst *
mono_find_spvar_for_region (MonoCompile *cfg, int region)
{
	region = mono_get_block_region_notry (cfg, region);

	return (MonoInst *)g_hash_table_lookup (cfg->spvars, GINT_TO_POINTER (region));
}

guint32
mono_reverse_branch_op (guint32 opcode)
{
	static const int reverse_map [] = {
		CEE_BNE_UN, CEE_BLT, CEE_BLE, CEE_BGT, CEE_BGE,
		CEE_BEQ, CEE_BLT_UN, CEE_BLE_UN, CEE_BGT_UN, CEE_BGE_UN
	};
	static const int reverse_fmap [] = {
		OP_FBNE_UN, OP_FBLT, OP_FBLE, OP_FBGT, OP_FBGE,
		OP_FBEQ, OP_FBLT_UN, OP_FBLE_UN, OP_FBGT_UN, OP_FBGE_UN
	};
	static const int reverse_lmap [] = {
		OP_LBNE_UN, OP_LBLT, OP_LBLE, OP_LBGT, OP_LBGE,
		OP_LBEQ, OP_LBLT_UN, OP_LBLE_UN, OP_LBGT_UN, OP_LBGE_UN
	};
	static const int reverse_imap [] = {
		OP_IBNE_UN, OP_IBLT, OP_IBLE, OP_IBGT, OP_IBGE,
		OP_IBEQ, OP_IBLT_UN, OP_IBLE_UN, OP_IBGT_UN, OP_IBGE_UN
	};
				
	if (opcode >= CEE_BEQ && opcode <= CEE_BLT_UN) {
		opcode = reverse_map [opcode - CEE_BEQ];
	} else if (opcode >= OP_FBEQ && opcode <= OP_FBLT_UN) {
		opcode = reverse_fmap [opcode - OP_FBEQ];
	} else if (opcode >= OP_LBEQ && opcode <= OP_LBLT_UN) {
		opcode = reverse_lmap [opcode - OP_LBEQ];
	} else if (opcode >= OP_IBEQ && opcode <= OP_IBLT_UN) {
		opcode = reverse_imap [opcode - OP_IBEQ];
	} else
		g_assert_not_reached ();

	return opcode;
}

guint
mono_type_to_store_membase (MonoCompile *cfg, MonoType *type)
{
	type = mini_get_underlying_type (type);

handle_enum:
	switch (type->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		return OP_STOREI1_MEMBASE_REG;
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		return OP_STOREI2_MEMBASE_REG;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return OP_STOREI4_MEMBASE_REG;
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		return OP_STORE_MEMBASE_REG;
	case MONO_TYPE_CLASS:
	case MONO_TYPE_STRING:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_ARRAY:    
		return OP_STORE_MEMBASE_REG;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return OP_STOREI8_MEMBASE_REG;
	case MONO_TYPE_R4:
		return OP_STORER4_MEMBASE_REG;
	case MONO_TYPE_R8:
		return OP_STORER8_MEMBASE_REG;
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (type->data.klass)) {
			type = mono_class_enum_basetype_internal (type->data.klass);
			goto handle_enum;
		}
		if (MONO_CLASS_IS_SIMD (cfg, mono_class_from_mono_type_internal (type)))
			return OP_STOREX_MEMBASE;
		return OP_STOREV_MEMBASE;
	case MONO_TYPE_TYPEDBYREF:
		return OP_STOREV_MEMBASE;
	case MONO_TYPE_GENERICINST:
		if (MONO_CLASS_IS_SIMD (cfg, mono_class_from_mono_type_internal (type)))
			return OP_STOREX_MEMBASE;
		type = m_class_get_byval_arg (type->data.generic_class->container_class);
		goto handle_enum;
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		g_assert (mini_type_var_is_vt (type));
		return OP_STOREV_MEMBASE;
	default:
		g_error ("unknown type 0x%02x in type_to_store_membase", type->type);
	}
	return -1;
}

guint
mono_type_to_load_membase (MonoCompile *cfg, MonoType *type)
{
	type = mini_get_underlying_type (type);

	switch (type->type) {
	case MONO_TYPE_I1:
		return OP_LOADI1_MEMBASE;
	case MONO_TYPE_U1:
		return OP_LOADU1_MEMBASE;
	case MONO_TYPE_I2:
		return OP_LOADI2_MEMBASE;
	case MONO_TYPE_U2:
		return OP_LOADU2_MEMBASE;
	case MONO_TYPE_I4:
		return OP_LOADI4_MEMBASE;
	case MONO_TYPE_U4:
		return OP_LOADU4_MEMBASE;
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		return OP_LOAD_MEMBASE;
	case MONO_TYPE_CLASS:
	case MONO_TYPE_STRING:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_ARRAY:    
		return OP_LOAD_MEMBASE;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return OP_LOADI8_MEMBASE;
	case MONO_TYPE_R4:
		return OP_LOADR4_MEMBASE;
	case MONO_TYPE_R8:
		return OP_LOADR8_MEMBASE;
	case MONO_TYPE_VALUETYPE:
		if (MONO_CLASS_IS_SIMD (cfg, mono_class_from_mono_type_internal (type)))
			return OP_LOADX_MEMBASE;
	case MONO_TYPE_TYPEDBYREF:
		return OP_LOADV_MEMBASE;
	case MONO_TYPE_GENERICINST:
		if (MONO_CLASS_IS_SIMD (cfg, mono_class_from_mono_type_internal (type)))
			return OP_LOADX_MEMBASE;
		if (mono_type_generic_inst_is_valuetype (type))
			return OP_LOADV_MEMBASE;
		else
			return OP_LOAD_MEMBASE;
		break;
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		g_assert (cfg->gshared);
		g_assert (mini_type_var_is_vt (type));
		return OP_LOADV_MEMBASE;
	default:
		g_error ("unknown type 0x%02x in type_to_load_membase", type->type);
	}
	return -1;
}

guint
mini_type_to_stind (MonoCompile* cfg, MonoType *type)
{
	type = mini_get_underlying_type (type);
	if (cfg->gshared && !type->byref && (type->type == MONO_TYPE_VAR || type->type == MONO_TYPE_MVAR)) {
		g_assert (mini_type_var_is_vt (type));
		return CEE_STOBJ;
	}
	return mono_type_to_stind (type);
}

int
mono_op_imm_to_op (int opcode)
{
	switch (opcode) {
	case OP_ADD_IMM:
#if SIZEOF_REGISTER == 4
		return OP_IADD;
#else
		return OP_LADD;
#endif
	case OP_IADD_IMM:
		return OP_IADD;
	case OP_LADD_IMM:
		return OP_LADD;
	case OP_ISUB_IMM:
		return OP_ISUB;
	case OP_LSUB_IMM:
		return OP_LSUB;
	case OP_IMUL_IMM:
		return OP_IMUL;
	case OP_LMUL_IMM:
		return OP_LMUL;
	case OP_AND_IMM:
#if SIZEOF_REGISTER == 4
		return OP_IAND;
#else
		return OP_LAND;
#endif
	case OP_OR_IMM:
#if SIZEOF_REGISTER == 4
		return OP_IOR;
#else
		return OP_LOR;
#endif
	case OP_XOR_IMM:
#if SIZEOF_REGISTER == 4
		return OP_IXOR;
#else
		return OP_LXOR;
#endif
	case OP_IAND_IMM:
		return OP_IAND;
	case OP_LAND_IMM:
		return OP_LAND;
	case OP_IOR_IMM:
		return OP_IOR;
	case OP_LOR_IMM:
		return OP_LOR;
	case OP_IXOR_IMM:
		return OP_IXOR;
	case OP_LXOR_IMM:
		return OP_LXOR;
	case OP_ISHL_IMM:
		return OP_ISHL;
	case OP_LSHL_IMM:
		return OP_LSHL;
	case OP_ISHR_IMM:
		return OP_ISHR;
	case OP_LSHR_IMM:
		return OP_LSHR;
	case OP_ISHR_UN_IMM:
		return OP_ISHR_UN;
	case OP_LSHR_UN_IMM:
		return OP_LSHR_UN;
	case OP_IDIV_IMM:
		return OP_IDIV;
	case OP_LDIV_IMM:
		return OP_LDIV;
	case OP_IDIV_UN_IMM:
		return OP_IDIV_UN;
	case OP_LDIV_UN_IMM:
		return OP_LDIV_UN;
	case OP_IREM_UN_IMM:
		return OP_IREM_UN;
	case OP_LREM_UN_IMM:
		return OP_LREM_UN;
	case OP_IREM_IMM:
		return OP_IREM;
	case OP_LREM_IMM:
		return OP_LREM;
	case OP_DIV_IMM:
#if SIZEOF_REGISTER == 4
		return OP_IDIV;
#else
		return OP_LDIV;
#endif
	case OP_REM_IMM:
#if SIZEOF_REGISTER == 4
		return OP_IREM;
#else
		return OP_LREM;
#endif
	case OP_ADDCC_IMM:
		return OP_ADDCC;
	case OP_ADC_IMM:
		return OP_ADC;
	case OP_SUBCC_IMM:
		return OP_SUBCC;
	case OP_SBB_IMM:
		return OP_SBB;
	case OP_IADC_IMM:
		return OP_IADC;
	case OP_ISBB_IMM:
		return OP_ISBB;
	case OP_COMPARE_IMM:
		return OP_COMPARE;
	case OP_ICOMPARE_IMM:
		return OP_ICOMPARE;
	case OP_LOCALLOC_IMM:
		return OP_LOCALLOC;
	}

	return -1;
}

/*
 * mono_decompose_op_imm:
 *
 *   Replace the OP_.._IMM INS with its non IMM variant.
 */
void
mono_decompose_op_imm (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins)
{
	int opcode2 = mono_op_imm_to_op (ins->opcode);
	MonoInst *temp;
	guint32 dreg;
	const char *spec = INS_INFO (ins->opcode);

	if (spec [MONO_INST_SRC2] == 'l') {
		dreg = mono_alloc_lreg (cfg);

		/* Load the 64bit constant using decomposed ops */
		MONO_INST_NEW (cfg, temp, OP_ICONST);
		temp->inst_c0 = ins_get_l_low (ins);
		temp->dreg = MONO_LVREG_LS (dreg);
		mono_bblock_insert_before_ins (bb, ins, temp);

		MONO_INST_NEW (cfg, temp, OP_ICONST);
		temp->inst_c0 = ins_get_l_high (ins);
		temp->dreg = MONO_LVREG_MS (dreg);
	} else {
		dreg = mono_alloc_ireg (cfg);

		MONO_INST_NEW (cfg, temp, OP_ICONST);
		temp->inst_c0 = ins->inst_imm;
		temp->dreg = dreg;
	}

	mono_bblock_insert_before_ins (bb, ins, temp);

	if (opcode2 == -1)
                g_error ("mono_op_imm_to_op failed for %s\n", mono_inst_name (ins->opcode));
	ins->opcode = opcode2;

	if (ins->opcode == OP_LOCALLOC)
		ins->sreg1 = dreg;
	else
		ins->sreg2 = dreg;

	bb->max_vreg = MAX (bb->max_vreg, cfg->next_vreg);
}

static void
set_vreg_to_inst (MonoCompile *cfg, int vreg, MonoInst *inst)
{
	if (vreg >= cfg->vreg_to_inst_len) {
		MonoInst **tmp = cfg->vreg_to_inst;
		int size = cfg->vreg_to_inst_len;

		while (vreg >= cfg->vreg_to_inst_len)
			cfg->vreg_to_inst_len = cfg->vreg_to_inst_len ? cfg->vreg_to_inst_len * 2 : 32;
		cfg->vreg_to_inst = (MonoInst **)mono_mempool_alloc0 (cfg->mempool, sizeof (MonoInst*) * cfg->vreg_to_inst_len);
		if (size)
			memcpy (cfg->vreg_to_inst, tmp, size * sizeof (MonoInst*));
	}
	cfg->vreg_to_inst [vreg] = inst;
}

#define mono_type_is_long(type) (!(type)->byref && ((mono_type_get_underlying_type (type)->type == MONO_TYPE_I8) || (mono_type_get_underlying_type (type)->type == MONO_TYPE_U8)))
#define mono_type_is_float(type) (!(type)->byref && (((type)->type == MONO_TYPE_R8) || ((type)->type == MONO_TYPE_R4)))

MonoInst*
mono_compile_create_var_for_vreg (MonoCompile *cfg, MonoType *type, int opcode, int vreg)
{
	MonoInst *inst;
	int num = cfg->num_varinfo;
	gboolean regpair;

	type = mini_get_underlying_type (type);

	if ((num + 1) >= cfg->varinfo_count) {
		int orig_count = cfg->varinfo_count;
		cfg->varinfo_count = cfg->varinfo_count ? (cfg->varinfo_count * 2) : 32;
		cfg->varinfo = (MonoInst **)g_realloc (cfg->varinfo, sizeof (MonoInst*) * cfg->varinfo_count);
		cfg->vars = (MonoMethodVar *)g_realloc (cfg->vars, sizeof (MonoMethodVar) * cfg->varinfo_count);
		memset (&cfg->vars [orig_count], 0, (cfg->varinfo_count - orig_count) * sizeof (MonoMethodVar));
	}

	cfg->stat_allocate_var++;

	MONO_INST_NEW (cfg, inst, opcode);
	inst->inst_c0 = num;
	inst->inst_vtype = type;
	inst->klass = mono_class_from_mono_type_internal (type);
	mini_type_to_eval_stack_type (cfg, type, inst);
	/* if set to 1 the variable is native */
	inst->backend.is_pinvoke = 0;
	inst->dreg = vreg;

	if (mono_class_has_failure (inst->klass)) {
		mono_cfg_set_exception (cfg, MONO_EXCEPTION_TYPE_LOAD);
		MonoErrorBoxed* box = mono_class_get_exception_data (inst->klass);
		if (box) {
			MonoErrorInternal* err = (MonoErrorInternal*)&box->error;
			cfg->exception_message = mono_error_get_message (err);
		}
	}

	if (cfg->compute_gc_maps) {
		if (type->byref) {
			mono_mark_vreg_as_mp (cfg, vreg);
		} else {
			if ((MONO_TYPE_ISSTRUCT (type) && m_class_has_references (inst->klass)) || mini_type_is_reference (type)) {
				inst->flags |= MONO_INST_GC_TRACK;
				mono_mark_vreg_as_ref (cfg, vreg);
			}
		}
	}
	
	cfg->varinfo [num] = inst;

	cfg->vars [num].idx = num;
	cfg->vars [num].vreg = vreg;
	cfg->vars [num].range.first_use.pos.bid = 0xffff;
	cfg->vars [num].reg = -1;

	if (vreg != -1)
		set_vreg_to_inst (cfg, vreg, inst);

#if SIZEOF_REGISTER == 4
	if (mono_arch_is_soft_float ()) {
		regpair = mono_type_is_long (type) || mono_type_is_float (type);
	} else {
		regpair = mono_type_is_long (type);
	}
#else
	regpair = FALSE;
#endif

	if (regpair) {
		MonoInst *tree;

		/* 
		 * These two cannot be allocated using create_var_for_vreg since that would
		 * put it into the cfg->varinfo array, confusing many parts of the JIT.
		 */

		/* 
		 * Set flags to VOLATILE so SSA skips it.
		 */

		if (cfg->verbose_level >= 4) {
			printf ("  Create LVAR R%d (R%d, R%d)\n", inst->dreg, MONO_LVREG_LS (inst->dreg), MONO_LVREG_MS (inst->dreg));
		}

		if (mono_arch_is_soft_float () && cfg->opt & MONO_OPT_SSA) {
			if (mono_type_is_float (type))
				inst->flags = MONO_INST_VOLATILE;
		}

		/* Allocate a dummy MonoInst for the first vreg */
		MONO_INST_NEW (cfg, tree, OP_LOCAL);
		tree->dreg = MONO_LVREG_LS (inst->dreg);
		if (cfg->opt & MONO_OPT_SSA)
			tree->flags = MONO_INST_VOLATILE;
		tree->inst_c0 = num;
		tree->type = STACK_I4;
		tree->inst_vtype = mono_get_int32_type ();
		tree->klass = mono_class_from_mono_type_internal (tree->inst_vtype);

		set_vreg_to_inst (cfg, MONO_LVREG_LS (inst->dreg), tree);

		/* Allocate a dummy MonoInst for the second vreg */
		MONO_INST_NEW (cfg, tree, OP_LOCAL);
		tree->dreg = MONO_LVREG_MS (inst->dreg);
		if (cfg->opt & MONO_OPT_SSA)
			tree->flags = MONO_INST_VOLATILE;
		tree->inst_c0 = num;
		tree->type = STACK_I4;
		tree->inst_vtype = mono_get_int32_type ();
		tree->klass = mono_class_from_mono_type_internal (tree->inst_vtype);

		set_vreg_to_inst (cfg, MONO_LVREG_MS (inst->dreg), tree);
	}

	cfg->num_varinfo++;
	if (cfg->verbose_level > 2)
		g_print ("created temp %d (R%d) of type %s\n", num, vreg, mono_type_get_name (type));

	return inst;
}

MonoInst*
mono_compile_create_var (MonoCompile *cfg, MonoType *type, int opcode)
{
	int dreg;

#ifdef ENABLE_NETCORE
	if (type->type == MONO_TYPE_VALUETYPE && !type->byref) {
		MonoClass *klass = mono_class_from_mono_type_internal (type);
		if (m_class_is_enumtype (klass) && m_class_get_image (klass) == mono_get_corlib () && !strcmp (m_class_get_name (klass), "StackCrawlMark")) {
			if (!(cfg->method->flags & METHOD_ATTRIBUTE_REQSECOBJ))
				g_error ("Method '%s' which contains a StackCrawlMark local variable must be decorated with [System.Security.DynamicSecurityMethod].", mono_method_get_full_name (cfg->method));
		}
	}
#endif

	type = mini_get_underlying_type (type);

	if (mono_type_is_long (type))
		dreg = mono_alloc_dreg (cfg, STACK_I8);
	else if (mono_arch_is_soft_float () && mono_type_is_float (type))
		dreg = mono_alloc_dreg (cfg, STACK_R8);
	else
		/* All the others are unified */
		dreg = mono_alloc_preg (cfg);

	return mono_compile_create_var_for_vreg (cfg, type, opcode, dreg);
}

MonoInst*
mini_get_int_to_float_spill_area (MonoCompile *cfg)
{
#ifdef TARGET_X86
	if (!cfg->iconv_raw_var) {
		cfg->iconv_raw_var = mono_compile_create_var (cfg, mono_get_int32_type (), OP_LOCAL);
		cfg->iconv_raw_var->flags |= MONO_INST_VOLATILE; /*FIXME, use the don't regalloc flag*/
	}
	return cfg->iconv_raw_var;
#else
	return NULL;
#endif
}

void
mono_mark_vreg_as_ref (MonoCompile *cfg, int vreg)
{
	if (vreg >= cfg->vreg_is_ref_len) {
		gboolean *tmp = cfg->vreg_is_ref;
		int size = cfg->vreg_is_ref_len;

		while (vreg >= cfg->vreg_is_ref_len)
			cfg->vreg_is_ref_len = cfg->vreg_is_ref_len ? cfg->vreg_is_ref_len * 2 : 32;
		cfg->vreg_is_ref = (gboolean *)mono_mempool_alloc0 (cfg->mempool, sizeof (gboolean) * cfg->vreg_is_ref_len);
		if (size)
			memcpy (cfg->vreg_is_ref, tmp, size * sizeof (gboolean));
	}
	cfg->vreg_is_ref [vreg] = TRUE;
}	

void
mono_mark_vreg_as_mp (MonoCompile *cfg, int vreg)
{
	if (vreg >= cfg->vreg_is_mp_len) {
		gboolean *tmp = cfg->vreg_is_mp;
		int size = cfg->vreg_is_mp_len;

		while (vreg >= cfg->vreg_is_mp_len)
			cfg->vreg_is_mp_len = cfg->vreg_is_mp_len ? cfg->vreg_is_mp_len * 2 : 32;
		cfg->vreg_is_mp = (gboolean *)mono_mempool_alloc0 (cfg->mempool, sizeof (gboolean) * cfg->vreg_is_mp_len);
		if (size)
			memcpy (cfg->vreg_is_mp, tmp, size * sizeof (gboolean));
	}
	cfg->vreg_is_mp [vreg] = TRUE;
}	

/*
 * mono_add_ins_to_end:
 *
 *   Same as MONO_ADD_INS, but add INST before any branches at the end of BB.
 */
void
mono_add_ins_to_end (MonoBasicBlock *bb, MonoInst *inst)
{
	int opcode;

	if (!bb->code) {
		MONO_ADD_INS (bb, inst);
		return;
	}

	switch (bb->last_ins->opcode) {
	case OP_BR:
	case OP_BR_REG:
	case CEE_BEQ:
	case CEE_BGE:
	case CEE_BGT:
	case CEE_BLE:
	case CEE_BLT:
	case CEE_BNE_UN:
	case CEE_BGE_UN:
	case CEE_BGT_UN:
	case CEE_BLE_UN:
	case CEE_BLT_UN:
	case OP_SWITCH:
		mono_bblock_insert_before_ins (bb, bb->last_ins, inst);
		break;
	default:
		if (MONO_IS_COND_BRANCH_OP (bb->last_ins)) {
			/* Need to insert the ins before the compare */
			if (bb->code == bb->last_ins) {
				mono_bblock_insert_before_ins (bb, bb->last_ins, inst);
				return;
			}

			if (bb->code->next == bb->last_ins) {
				/* Only two instructions */
				opcode = bb->code->opcode;

				if ((opcode == OP_COMPARE) || (opcode == OP_COMPARE_IMM) || (opcode == OP_ICOMPARE) || (opcode == OP_ICOMPARE_IMM) || (opcode == OP_FCOMPARE) || (opcode == OP_LCOMPARE) || (opcode == OP_LCOMPARE_IMM) || (opcode == OP_RCOMPARE)) {
					/* NEW IR */
					mono_bblock_insert_before_ins (bb, bb->code, inst);
				} else {
					mono_bblock_insert_before_ins (bb, bb->last_ins, inst);
				}
			} else {
				opcode = bb->last_ins->prev->opcode;

				if ((opcode == OP_COMPARE) || (opcode == OP_COMPARE_IMM) || (opcode == OP_ICOMPARE) || (opcode == OP_ICOMPARE_IMM) || (opcode == OP_FCOMPARE) || (opcode == OP_LCOMPARE) || (opcode == OP_LCOMPARE_IMM) || (opcode == OP_RCOMPARE)) {
					/* NEW IR */
					mono_bblock_insert_before_ins (bb, bb->last_ins->prev, inst);
				} else {
					mono_bblock_insert_before_ins (bb, bb->last_ins, inst);
				}					
			}
		}
		else
			MONO_ADD_INS (bb, inst);
		break;
	}
}

gboolean
mini_assembly_can_skip_verification (MonoDomain *domain, MonoMethod *method)
{
	MonoAssembly *assembly = m_class_get_image (method->klass)->assembly;
	if (method->wrapper_type != MONO_WRAPPER_NONE && method->wrapper_type != MONO_WRAPPER_DYNAMIC_METHOD)
		return FALSE;
	if (assembly->in_gac || assembly->image == mono_defaults.corlib)
		return FALSE;
	return mono_assembly_has_skip_verification (assembly);
}

/*
 * mini_method_verify:
 * 
 * Verify the method using the verfier.
 * 
 * Returns true if the method is invalid. 
 */
static gboolean
mini_method_verify (MonoCompile *cfg, MonoMethod *method, gboolean fail_compile)
{
	GSList *tmp, *res;
	gboolean is_fulltrust;

	if (mono_method_get_verification_success (method))
		return FALSE;

	if (!mono_verifier_is_enabled_for_method (method))
		return FALSE;

	/*skip verification implies the assembly must be */
	is_fulltrust = mono_verifier_is_method_full_trust (method) ||  mini_assembly_can_skip_verification (cfg->domain, method);

	res = mono_method_verify_with_current_settings (method, cfg->skip_visibility, is_fulltrust);

	if (res) { 
		for (tmp = res; tmp; tmp = tmp->next) {
			MonoVerifyInfoExtended *info = (MonoVerifyInfoExtended *)tmp->data;
			if (info->info.status == MONO_VERIFY_ERROR) {
				if (fail_compile) {
				char *method_name = mono_method_full_name (method, TRUE);
					cfg->exception_type = (MonoExceptionType)info->exception_type;
					cfg->exception_message = g_strdup_printf ("Error verifying %s: %s", method_name, info->info.message);
					g_free (method_name);
				}
				mono_free_verify_list (res);
				return TRUE;
			}
			if (info->info.status == MONO_VERIFY_NOT_VERIFIABLE && (!is_fulltrust || info->exception_type == MONO_EXCEPTION_METHOD_ACCESS || info->exception_type == MONO_EXCEPTION_FIELD_ACCESS)) {
				if (fail_compile) {
					char *method_name = mono_method_full_name (method, TRUE);
					char *msg = g_strdup_printf ("Error verifying %s: %s", method_name, info->info.message);

					if (info->exception_type == MONO_EXCEPTION_METHOD_ACCESS)
						mono_error_set_generic_error (cfg->error, "System", "MethodAccessException", "%s", msg);
					else if (info->exception_type == MONO_EXCEPTION_FIELD_ACCESS)
						mono_error_set_generic_error (cfg->error, "System", "FieldAccessException", "%s", msg);
					else if (info->exception_type == MONO_EXCEPTION_UNVERIFIABLE_IL)
						mono_error_set_generic_error (cfg->error, "System.Security", "VerificationException", "%s", msg);
					if (!is_ok (cfg->error)) {
						mono_cfg_set_exception (cfg, MONO_EXCEPTION_MONO_ERROR);
						g_free (msg);
					} else {
						cfg->exception_type = (MonoExceptionType)info->exception_type;
						cfg->exception_message = msg;
					}
					g_free (method_name);
				}
				mono_free_verify_list (res);
				return TRUE;
			}
		}
		mono_free_verify_list (res);
	}
	mono_method_set_verification_success (method);
	return FALSE;
}

/*Returns true if something went wrong*/
gboolean
mono_compile_is_broken (MonoCompile *cfg, MonoMethod *method, gboolean fail_compile)
{
	MonoMethod *method_definition = method;
	gboolean dont_verify = m_class_get_image (method->klass)->assembly->corlib_internal;

	while (method_definition->is_inflated) {
		MonoMethodInflated *imethod = (MonoMethodInflated *) method_definition;
		method_definition = imethod->declaring;
	}

	return !dont_verify && mini_method_verify (cfg, method_definition, fail_compile);
}

typedef struct {
	MonoClass *vtype;
	GList *active, *inactive;
	GSList *slots;
} StackSlotInfo;

static gint 
compare_by_interval_start_pos_func (gconstpointer a, gconstpointer b)
{
	MonoMethodVar *v1 = (MonoMethodVar*)a;
	MonoMethodVar *v2 = (MonoMethodVar*)b;

	if (v1 == v2)
		return 0;
	else if (v1->interval->range && v2->interval->range)
		return v1->interval->range->from - v2->interval->range->from;
	else if (v1->interval->range)
		return -1;
	else
		return 1;
}

#if 0
#define LSCAN_DEBUG(a) do { a; } while (0)
#else
#define LSCAN_DEBUG(a) do { } while (0) /* non-empty to avoid warning */
#endif

static gint32*
mono_allocate_stack_slots2 (MonoCompile *cfg, gboolean backward, guint32 *stack_size, guint32 *stack_align)
{
	int i, slot, offset, size;
	guint32 align;
	MonoMethodVar *vmv;
	MonoInst *inst;
	gint32 *offsets;
	GList *vars = NULL, *l, *unhandled;
	StackSlotInfo *scalar_stack_slots, *vtype_stack_slots, *slot_info;
	MonoType *t;
	int nvtypes;
	int vtype_stack_slots_size = 256;
	gboolean reuse_slot;

	LSCAN_DEBUG (printf ("Allocate Stack Slots 2 for %s:\n", mono_method_full_name (cfg->method, TRUE)));

	scalar_stack_slots = (StackSlotInfo *)mono_mempool_alloc0 (cfg->mempool, sizeof (StackSlotInfo) * MONO_TYPE_PINNED);
	vtype_stack_slots = NULL;
	nvtypes = 0;

	offsets = (gint32 *)mono_mempool_alloc (cfg->mempool, sizeof (gint32) * cfg->num_varinfo);
	for (i = 0; i < cfg->num_varinfo; ++i)
		offsets [i] = -1;

	for (i = cfg->locals_start; i < cfg->num_varinfo; i++) {
		inst = cfg->varinfo [i];
		vmv = MONO_VARINFO (cfg, i);

		if ((inst->flags & MONO_INST_IS_DEAD) || inst->opcode == OP_REGVAR || inst->opcode == OP_REGOFFSET)
			continue;

		vars = g_list_prepend (vars, vmv);
	}

	vars = g_list_sort (vars, compare_by_interval_start_pos_func);

	/* Sanity check */
	/*
	i = 0;
	for (unhandled = vars; unhandled; unhandled = unhandled->next) {
		MonoMethodVar *current = unhandled->data;

		if (current->interval->range) {
			g_assert (current->interval->range->from >= i);
			i = current->interval->range->from;
		}
	}
	*/

	offset = 0;
	*stack_align = 0;
	for (unhandled = vars; unhandled; unhandled = unhandled->next) {
		MonoMethodVar *current = (MonoMethodVar *)unhandled->data;

		vmv = current;
		inst = cfg->varinfo [vmv->idx];

		t = mono_type_get_underlying_type (inst->inst_vtype);
		if (cfg->gsharedvt && mini_is_gsharedvt_variable_type (t))
			continue;

		/* inst->backend.is_pinvoke indicates native sized value types, this is used by the
		* pinvoke wrappers when they call functions returning structures */
		if (inst->backend.is_pinvoke && MONO_TYPE_ISSTRUCT (t) && t->type != MONO_TYPE_TYPEDBYREF) {
			size = mono_class_native_size (mono_class_from_mono_type_internal (t), &align);
		}
		else {
			int ialign;

			size = mini_type_stack_size (t, &ialign);
			align = ialign;

			if (MONO_CLASS_IS_SIMD (cfg, mono_class_from_mono_type_internal (t)))
				align = 16;
		}

		reuse_slot = TRUE;
		if (cfg->disable_reuse_stack_slots)
			reuse_slot = FALSE;

		t = mini_get_underlying_type (t);
		switch (t->type) {
		case MONO_TYPE_GENERICINST:
			if (!mono_type_generic_inst_is_valuetype (t)) {
				slot_info = &scalar_stack_slots [t->type];
				break;
			}
			/* Fall through */
		case MONO_TYPE_VALUETYPE:
			if (!vtype_stack_slots)
				vtype_stack_slots = (StackSlotInfo *)mono_mempool_alloc0 (cfg->mempool, sizeof (StackSlotInfo) * vtype_stack_slots_size);
			for (i = 0; i < nvtypes; ++i)
				if (t->data.klass == vtype_stack_slots [i].vtype)
					break;
			if (i < nvtypes)
				slot_info = &vtype_stack_slots [i];
			else {
				if (nvtypes == vtype_stack_slots_size) {
					int new_slots_size = vtype_stack_slots_size * 2;
					StackSlotInfo* new_slots = (StackSlotInfo *)mono_mempool_alloc0 (cfg->mempool, sizeof (StackSlotInfo) * new_slots_size);

					memcpy (new_slots, vtype_stack_slots, sizeof (StackSlotInfo) * vtype_stack_slots_size);

					vtype_stack_slots = new_slots;
					vtype_stack_slots_size = new_slots_size;
				}
				vtype_stack_slots [nvtypes].vtype = t->data.klass;
				slot_info = &vtype_stack_slots [nvtypes];
				nvtypes ++;
			}
			if (cfg->disable_reuse_ref_stack_slots)
				reuse_slot = FALSE;
			break;

		case MONO_TYPE_PTR:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
#if TARGET_SIZEOF_VOID_P == 4
		case MONO_TYPE_I4:
#else
		case MONO_TYPE_I8:
#endif
			if (cfg->disable_ref_noref_stack_slot_share) {
				slot_info = &scalar_stack_slots [MONO_TYPE_I];
				break;
			}
			/* Fall through */

		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_ARRAY:
		case MONO_TYPE_SZARRAY:
		case MONO_TYPE_STRING:
			/* Share non-float stack slots of the same size */
			slot_info = &scalar_stack_slots [MONO_TYPE_CLASS];
			if (cfg->disable_reuse_ref_stack_slots)
				reuse_slot = FALSE;
			break;

		default:
			slot_info = &scalar_stack_slots [t->type];
		}

		slot = 0xffffff;
		if (cfg->comp_done & MONO_COMP_LIVENESS) {
			int pos;
			gboolean changed;

			//printf ("START  %2d %08x %08x\n",  vmv->idx, vmv->range.first_use.abs_pos, vmv->range.last_use.abs_pos);

			if (!current->interval->range) {
				if (inst->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT))
					pos = ~0;
				else {
					/* Dead */
					inst->flags |= MONO_INST_IS_DEAD;
					continue;
				}
			}
			else
				pos = current->interval->range->from;

			LSCAN_DEBUG (printf ("process R%d ", inst->dreg));
			if (current->interval->range)
				LSCAN_DEBUG (mono_linterval_print (current->interval));
			LSCAN_DEBUG (printf ("\n"));

			/* Check for intervals in active which expired or inactive */
			changed = TRUE;
			/* FIXME: Optimize this */
			while (changed) {
				changed = FALSE;
				for (l = slot_info->active; l != NULL; l = l->next) {
					MonoMethodVar *v = (MonoMethodVar*)l->data;

					if (v->interval->last_range->to < pos) {
						slot_info->active = g_list_delete_link (slot_info->active, l);
						slot_info->slots = g_slist_prepend_mempool (cfg->mempool, slot_info->slots, GINT_TO_POINTER (offsets [v->idx]));
						LSCAN_DEBUG (printf ("Interval R%d has expired, adding 0x%x to slots\n", cfg->varinfo [v->idx]->dreg, offsets [v->idx]));
						changed = TRUE;
						break;
					}
					else if (!mono_linterval_covers (v->interval, pos)) {
						slot_info->inactive = g_list_append (slot_info->inactive, v);
						slot_info->active = g_list_delete_link (slot_info->active, l);
						LSCAN_DEBUG (printf ("Interval R%d became inactive\n", cfg->varinfo [v->idx]->dreg));
						changed = TRUE;
						break;
					}
				}
			}

			/* Check for intervals in inactive which expired or active */
			changed = TRUE;
			/* FIXME: Optimize this */
			while (changed) {
				changed = FALSE;
				for (l = slot_info->inactive; l != NULL; l = l->next) {
					MonoMethodVar *v = (MonoMethodVar*)l->data;

					if (v->interval->last_range->to < pos) {
						slot_info->inactive = g_list_delete_link (slot_info->inactive, l);
						// FIXME: Enabling this seems to cause impossible to debug crashes
						//slot_info->slots = g_slist_prepend_mempool (cfg->mempool, slot_info->slots, GINT_TO_POINTER (offsets [v->idx]));
						LSCAN_DEBUG (printf ("Interval R%d has expired, adding 0x%x to slots\n", cfg->varinfo [v->idx]->dreg, offsets [v->idx]));
						changed = TRUE;
						break;
					}
					else if (mono_linterval_covers (v->interval, pos)) {
						slot_info->active = g_list_append (slot_info->active, v);
						slot_info->inactive = g_list_delete_link (slot_info->inactive, l);
						LSCAN_DEBUG (printf ("\tInterval R%d became active\n", cfg->varinfo [v->idx]->dreg));
						changed = TRUE;
						break;
					}
				}
			}

			/* 
			 * This also handles the case when the variable is used in an
			 * exception region, as liveness info is not computed there.
			 */
			/* 
			 * FIXME: All valuetypes are marked as INDIRECT because of LDADDR
			 * opcodes.
			 */
			if (! (inst->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT))) {
				if (slot_info->slots) {
					slot = GPOINTER_TO_INT (slot_info->slots->data);

					slot_info->slots = slot_info->slots->next;
				}

				/* FIXME: We might want to consider the inactive intervals as well if slot_info->slots is empty */

				slot_info->active = mono_varlist_insert_sorted (cfg, slot_info->active, vmv, TRUE);
			}
		}

#if 0
		{
			static int count = 0;
			count ++;

			if (count == atoi (g_getenv ("COUNT3")))
				printf ("LAST: %s\n", mono_method_full_name (cfg->method, TRUE));
			if (count > atoi (g_getenv ("COUNT3")))
				slot = 0xffffff;
			else
				mono_print_ins (inst);
		}
#endif

		LSCAN_DEBUG (printf ("R%d %s -> 0x%x\n", inst->dreg, mono_type_full_name (t), slot));

		if (inst->flags & MONO_INST_LMF) {
			size = MONO_ABI_SIZEOF (MonoLMF);
			align = sizeof (target_mgreg_t);
			reuse_slot = FALSE;
		}

		if (!reuse_slot)
			slot = 0xffffff;

		if (slot == 0xffffff) {
			/*
			 * Allways allocate valuetypes to sizeof (target_mgreg_t) to allow more
			 * efficient copying (and to work around the fact that OP_MEMCPY
			 * and OP_MEMSET ignores alignment).
			 */
			if (MONO_TYPE_ISSTRUCT (t)) {
				align = MAX (align, sizeof (target_mgreg_t));
				align = MAX (align, mono_class_min_align (mono_class_from_mono_type_internal (t)));
			}

			if (backward) {
				offset += size;
				offset += align - 1;
				offset &= ~(align - 1);
				slot = offset;
			}
			else {
				offset += align - 1;
				offset &= ~(align - 1);
				slot = offset;
				offset += size;
			}

			if (*stack_align == 0)
				*stack_align = align;
		}

		offsets [vmv->idx] = slot;
	}
	g_list_free (vars);
	for (i = 0; i < MONO_TYPE_PINNED; ++i) {
		if (scalar_stack_slots [i].active)
			g_list_free (scalar_stack_slots [i].active);
	}
	for (i = 0; i < nvtypes; ++i) {
		if (vtype_stack_slots [i].active)
			g_list_free (vtype_stack_slots [i].active);
	}

	cfg->stat_locals_stack_size += offset;

	*stack_size = offset;
	return offsets;
}

/*
 *  mono_allocate_stack_slots:
 *
 *  Allocate stack slots for all non register allocated variables using a
 * linear scan algorithm.
 * Returns: an array of stack offsets.
 * STACK_SIZE is set to the amount of stack space needed.
 * STACK_ALIGN is set to the alignment needed by the locals area.
 */
gint32*
mono_allocate_stack_slots (MonoCompile *cfg, gboolean backward, guint32 *stack_size, guint32 *stack_align)
{
	int i, slot, offset, size;
	guint32 align;
	MonoMethodVar *vmv;
	MonoInst *inst;
	gint32 *offsets;
	GList *vars = NULL, *l;
	StackSlotInfo *scalar_stack_slots, *vtype_stack_slots, *slot_info;
	MonoType *t;
	int nvtypes;
	int vtype_stack_slots_size = 256;
	gboolean reuse_slot;

	if ((cfg->num_varinfo > 0) && MONO_VARINFO (cfg, 0)->interval)
		return mono_allocate_stack_slots2 (cfg, backward, stack_size, stack_align);

	scalar_stack_slots = (StackSlotInfo *)mono_mempool_alloc0 (cfg->mempool, sizeof (StackSlotInfo) * MONO_TYPE_PINNED);
	vtype_stack_slots = NULL;
	nvtypes = 0;

	offsets = (gint32 *)mono_mempool_alloc (cfg->mempool, sizeof (gint32) * cfg->num_varinfo);
	for (i = 0; i < cfg->num_varinfo; ++i)
		offsets [i] = -1;

	for (i = cfg->locals_start; i < cfg->num_varinfo; i++) {
		inst = cfg->varinfo [i];
		vmv = MONO_VARINFO (cfg, i);

		if ((inst->flags & MONO_INST_IS_DEAD) || inst->opcode == OP_REGVAR || inst->opcode == OP_REGOFFSET)
			continue;

		vars = g_list_prepend (vars, vmv);
	}

	vars = mono_varlist_sort (cfg, vars, 0);
	offset = 0;
	*stack_align = sizeof (target_mgreg_t);
	for (l = vars; l; l = l->next) {
		vmv = (MonoMethodVar *)l->data;
		inst = cfg->varinfo [vmv->idx];

		t = mono_type_get_underlying_type (inst->inst_vtype);
		if (cfg->gsharedvt && mini_is_gsharedvt_variable_type (t))
			continue;

		/* inst->backend.is_pinvoke indicates native sized value types, this is used by the
		* pinvoke wrappers when they call functions returning structures */
		if (inst->backend.is_pinvoke && MONO_TYPE_ISSTRUCT (t) && t->type != MONO_TYPE_TYPEDBYREF) {
			size = mono_class_native_size (mono_class_from_mono_type_internal (t), &align);
		} else {
			int ialign;

			size = mini_type_stack_size (t, &ialign);
			align = ialign;

			if (mono_class_has_failure (mono_class_from_mono_type_internal (t)))
				mono_cfg_set_exception (cfg, MONO_EXCEPTION_TYPE_LOAD);

			if (MONO_CLASS_IS_SIMD (cfg, mono_class_from_mono_type_internal (t)))
				align = 16;
		}

		reuse_slot = TRUE;
		if (cfg->disable_reuse_stack_slots)
			reuse_slot = FALSE;

		t = mini_get_underlying_type (t);
		switch (t->type) {
		case MONO_TYPE_GENERICINST:
			if (!mono_type_generic_inst_is_valuetype (t)) {
				slot_info = &scalar_stack_slots [t->type];
				break;
			}
			/* Fall through */
		case MONO_TYPE_VALUETYPE:
			if (!vtype_stack_slots)
				vtype_stack_slots = (StackSlotInfo *)mono_mempool_alloc0 (cfg->mempool, sizeof (StackSlotInfo) * vtype_stack_slots_size);
			for (i = 0; i < nvtypes; ++i)
				if (t->data.klass == vtype_stack_slots [i].vtype)
					break;
			if (i < nvtypes)
				slot_info = &vtype_stack_slots [i];
			else {
				if (nvtypes == vtype_stack_slots_size) {
					int new_slots_size = vtype_stack_slots_size * 2;
					StackSlotInfo* new_slots = (StackSlotInfo *)mono_mempool_alloc0 (cfg->mempool, sizeof (StackSlotInfo) * new_slots_size);

					memcpy (new_slots, vtype_stack_slots, sizeof (StackSlotInfo) * vtype_stack_slots_size);

					vtype_stack_slots = new_slots;
					vtype_stack_slots_size = new_slots_size;
				}
				vtype_stack_slots [nvtypes].vtype = t->data.klass;
				slot_info = &vtype_stack_slots [nvtypes];
				nvtypes ++;
			}
			if (cfg->disable_reuse_ref_stack_slots)
				reuse_slot = FALSE;
			break;

		case MONO_TYPE_PTR:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
#if TARGET_SIZEOF_VOID_P == 4
		case MONO_TYPE_I4:
#else
		case MONO_TYPE_I8:
#endif
			if (cfg->disable_ref_noref_stack_slot_share) {
				slot_info = &scalar_stack_slots [MONO_TYPE_I];
				break;
			}
			/* Fall through */

		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_ARRAY:
		case MONO_TYPE_SZARRAY:
		case MONO_TYPE_STRING:
			/* Share non-float stack slots of the same size */
			slot_info = &scalar_stack_slots [MONO_TYPE_CLASS];
			if (cfg->disable_reuse_ref_stack_slots)
				reuse_slot = FALSE;
			break;
		case MONO_TYPE_VAR:
		case MONO_TYPE_MVAR:
			slot_info = &scalar_stack_slots [t->type];
			break;
		default:
			slot_info = &scalar_stack_slots [t->type];
			break;
		}

		slot = 0xffffff;
		if (cfg->comp_done & MONO_COMP_LIVENESS) {
			//printf ("START  %2d %08x %08x\n",  vmv->idx, vmv->range.first_use.abs_pos, vmv->range.last_use.abs_pos);
			
			/* expire old intervals in active */
			while (slot_info->active) {
				MonoMethodVar *amv = (MonoMethodVar *)slot_info->active->data;

				if (amv->range.last_use.abs_pos > vmv->range.first_use.abs_pos)
					break;

				//printf ("EXPIR  %2d %08x %08x C%d R%d\n", amv->idx, amv->range.first_use.abs_pos, amv->range.last_use.abs_pos, amv->spill_costs, amv->reg);

				slot_info->active = g_list_delete_link (slot_info->active, slot_info->active);
				slot_info->slots = g_slist_prepend_mempool (cfg->mempool, slot_info->slots, GINT_TO_POINTER (offsets [amv->idx]));
			}

			/* 
			 * This also handles the case when the variable is used in an
			 * exception region, as liveness info is not computed there.
			 */
			/* 
			 * FIXME: All valuetypes are marked as INDIRECT because of LDADDR
			 * opcodes.
			 */
			if (! (inst->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT))) {
				if (slot_info->slots) {
					slot = GPOINTER_TO_INT (slot_info->slots->data);

					slot_info->slots = slot_info->slots->next;
				}

				slot_info->active = mono_varlist_insert_sorted (cfg, slot_info->active, vmv, TRUE);
			}
		}

#if 0
		{
			static int count = 0;
			count ++;

			if (count == atoi (g_getenv ("COUNT")))
				printf ("LAST: %s\n", mono_method_full_name (cfg->method, TRUE));
			if (count > atoi (g_getenv ("COUNT")))
				slot = 0xffffff;
			else
				mono_print_ins (inst);
		}
#endif

		if (inst->flags & MONO_INST_LMF) {
			/*
			 * This variable represents a MonoLMF structure, which has no corresponding
			 * CLR type, so hard-code its size/alignment.
			 */
			size = MONO_ABI_SIZEOF (MonoLMF);
			align = sizeof (target_mgreg_t);
			reuse_slot = FALSE;
		}

		if (!reuse_slot)
			slot = 0xffffff;

		if (slot == 0xffffff) {
			/*
			 * Allways allocate valuetypes to sizeof (target_mgreg_t) to allow more
			 * efficient copying (and to work around the fact that OP_MEMCPY
			 * and OP_MEMSET ignores alignment).
			 */
			if (MONO_TYPE_ISSTRUCT (t)) {
				align = MAX (align, sizeof (target_mgreg_t));
				align = MAX (align, mono_class_min_align (mono_class_from_mono_type_internal (t)));
				/* 
				 * Align the size too so the code generated for passing vtypes in
				 * registers doesn't overwrite random locals.
				 */
				size = (size + (align - 1)) & ~(align -1);
			}

			if (backward) {
				offset += size;
				offset += align - 1;
				offset &= ~(align - 1);
				slot = offset;
			}
			else {
				offset += align - 1;
				offset &= ~(align - 1);
				slot = offset;
				offset += size;
			}

			*stack_align = MAX (*stack_align, align);
		}

		offsets [vmv->idx] = slot;
	}
	g_list_free (vars);
	for (i = 0; i < MONO_TYPE_PINNED; ++i) {
		if (scalar_stack_slots [i].active)
			g_list_free (scalar_stack_slots [i].active);
	}
	for (i = 0; i < nvtypes; ++i) {
		if (vtype_stack_slots [i].active)
			g_list_free (vtype_stack_slots [i].active);
	}

	cfg->stat_locals_stack_size += offset;

	*stack_size = offset;
	return offsets;
}

#define EMUL_HIT_SHIFT 3
#define EMUL_HIT_MASK ((1 << EMUL_HIT_SHIFT) - 1)
/* small hit bitmap cache */
static mono_byte emul_opcode_hit_cache [(OP_LAST>>EMUL_HIT_SHIFT) + 1] = {0};
static short emul_opcode_num = 0;
static short emul_opcode_alloced = 0;
static short *emul_opcode_opcodes;
static MonoJitICallInfo **emul_opcode_map;

MonoJitICallInfo *
mono_find_jit_opcode_emulation (int opcode)
{
	g_assert (opcode >= 0 && opcode <= OP_LAST);
	if (emul_opcode_hit_cache [opcode >> (EMUL_HIT_SHIFT + 3)] & (1 << (opcode & EMUL_HIT_MASK))) {
		int i;
		for (i = 0; i < emul_opcode_num; ++i) {
			if (emul_opcode_opcodes [i] == opcode)
				return emul_opcode_map [i];
		}
	}
	return NULL;
}

void
mini_register_opcode_emulation (int opcode, MonoJitICallInfo *info, const char *name, MonoMethodSignature *sig, gpointer func, const char *symbol, gboolean no_wrapper)
{
	g_assert (info);
	g_assert (!sig->hasthis);
	g_assert (sig->param_count < 3);

	mono_register_jit_icall_info (info, func, name, sig, no_wrapper, symbol);

	if (emul_opcode_num >= emul_opcode_alloced) {
		int incr = emul_opcode_alloced? emul_opcode_alloced/2: 16;
		emul_opcode_alloced += incr;
		emul_opcode_map = (MonoJitICallInfo **)g_realloc (emul_opcode_map, sizeof (emul_opcode_map [0]) * emul_opcode_alloced);
		emul_opcode_opcodes = (short *)g_realloc (emul_opcode_opcodes, sizeof (emul_opcode_opcodes [0]) * emul_opcode_alloced);
	}
	emul_opcode_map [emul_opcode_num] = info;
	emul_opcode_opcodes [emul_opcode_num] = opcode;
	emul_opcode_num++;
	emul_opcode_hit_cache [opcode >> (EMUL_HIT_SHIFT + 3)] |= (1 << (opcode & EMUL_HIT_MASK));
}

void
mono_bblock_insert_after_ins (MonoBasicBlock *bb, MonoInst *ins, MonoInst *ins_to_insert)
{
	if (ins == NULL) {
		ins = bb->code;
		bb->code = ins_to_insert;

		/* Link with next */
		ins_to_insert->next = ins;
		if (ins)
			ins->prev = ins_to_insert;

		if (bb->last_ins == NULL)
			bb->last_ins = ins_to_insert;
	} else {
		/* Link with next */
		ins_to_insert->next = ins->next;
		if (ins->next)
			ins->next->prev = ins_to_insert;

		/* Link with previous */
		ins->next = ins_to_insert;
		ins_to_insert->prev = ins;

		if (bb->last_ins == ins)
			bb->last_ins = ins_to_insert;
	}
}

void
mono_bblock_insert_before_ins (MonoBasicBlock *bb, MonoInst *ins, MonoInst *ins_to_insert)
{
	if (ins == NULL) {
		ins = bb->code;
		if (ins)
			ins->prev = ins_to_insert;
		bb->code = ins_to_insert;
		ins_to_insert->next = ins;
		if (bb->last_ins == NULL)
			bb->last_ins = ins_to_insert;
	} else {
		/* Link with previous */
		if (ins->prev)
			ins->prev->next = ins_to_insert;
		ins_to_insert->prev = ins->prev;

		/* Link with next */
		ins->prev = ins_to_insert;
		ins_to_insert->next = ins;

		if (bb->code == ins)
			bb->code = ins_to_insert;
	}
}

// This will free many fields in cfg to save
// memory. Note that this must be safe to call
// multiple times. It must be idempotent. 

void
mono_add_patch_info (MonoCompile *cfg, int ip, MonoJumpInfoType type, gconstpointer target)
{
	if (type == MONO_PATCH_INFO_NONE)
		return;

	MonoJumpInfo *ji = (MonoJumpInfo *)mono_mempool_alloc0 (cfg->mempool, sizeof (MonoJumpInfo));

	ji->ip.i = ip;
	ji->type = type;
	ji->data.target = target;
	ji->next = cfg->patch_info;

	cfg->patch_info = ji;
}

void
mono_add_seq_point (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins, int native_offset)
{
	ins->inst_offset = native_offset;
	g_ptr_array_add (cfg->seq_points, ins);
	if (bb) {
		bb->seq_points = g_slist_prepend_mempool (cfg->mempool, bb->seq_points, ins);
		bb->last_seq_point = ins;
	}
}

void
mono_add_var_location (MonoCompile *cfg, MonoInst *var, gboolean is_reg, int reg, int offset, int from, int to)
{
	MonoDwarfLocListEntry *entry = (MonoDwarfLocListEntry *)mono_mempool_alloc0 (cfg->mempool, sizeof (MonoDwarfLocListEntry));

	if (is_reg)
		g_assert (offset == 0);

	entry->is_reg = is_reg;
	entry->reg = reg;
	entry->offset = offset;
	entry->from = from;
	entry->to = to;

	if (var == cfg->args [0])
		cfg->this_loclist = g_slist_append_mempool (cfg->mempool, cfg->this_loclist, entry);
	else if (var == cfg->rgctx_var)
		cfg->rgctx_loclist = g_slist_append_mempool (cfg->mempool, cfg->rgctx_loclist, entry);
}

void
mono_print_code (MonoCompile *cfg, const char* msg)
{
	MonoBasicBlock *bb;
	
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb)
		mono_print_bb (bb, msg);
}

gboolean
mini_class_has_reference_variant_generic_argument (MonoCompile *cfg, MonoClass *klass, int context_used)
{
	int i;
	MonoGenericContainer *container;
	MonoGenericInst *ginst;

	if (mono_class_is_ginst (klass)) {
		container = mono_class_get_generic_container (mono_class_get_generic_class (klass)->container_class);
		ginst = mono_class_get_generic_class (klass)->context.class_inst;
	} else if (mono_class_is_gtd (klass) && context_used) {
		container = mono_class_get_generic_container (klass);
		ginst = container->context.class_inst;
	} else {
		return FALSE;
	}

	for (i = 0; i < container->type_argc; ++i) {
		MonoType *type;
		if (!(mono_generic_container_get_param_info (container, i)->flags & (MONO_GEN_PARAM_VARIANT|MONO_GEN_PARAM_COVARIANT)))
			continue;
		type = ginst->type_argv [i];
		if (mini_type_is_reference (type))
			return TRUE;
	}
	return FALSE;
}

void
mono_cfg_add_try_hole (MonoCompile *cfg, MonoExceptionClause *clause, guint8 *start, MonoBasicBlock *bb)
{
	TryBlockHole *hole = (TryBlockHole *)mono_mempool_alloc (cfg->mempool, sizeof (TryBlockHole));
	hole->clause = clause;
	hole->start_offset = start - cfg->native_code;
	hole->basic_block = bb;

	cfg->try_block_holes = g_slist_append_mempool (cfg->mempool, cfg->try_block_holes, hole);
}

void
mono_cfg_set_exception (MonoCompile *cfg, MonoExceptionType type)
{
	cfg->exception_type = type;
}

/* Assumes ownership of the MSG argument */
void
mono_cfg_set_exception_invalid_program (MonoCompile *cfg, char *msg)
{
	mono_cfg_set_exception (cfg, MONO_EXCEPTION_MONO_ERROR);
	mono_error_set_generic_error (cfg->error, "System", "InvalidProgramException", "%s", msg);
}

#endif /* DISABLE_JIT */

gint64 mono_time_track_start ()
{
	return mono_100ns_ticks ();
}

/*
 * mono_time_track_end:
 *
 *   Uses UnlockedAddDouble () to update \param time.
 */
void mono_time_track_end (gint64 *time, gint64 start)
{
	UnlockedAdd64 (time, mono_100ns_ticks () - start);
}

/*
 * mini_get_underlying_type:
 *
 *   Return the type the JIT will use during compilation.
 * Handles: byref, enums, native types, bool/char, ref types, generic sharing.
 * For gsharedvt types, it will return the original VAR/MVAR.
 */
MonoType*
mini_get_underlying_type (MonoType *type)
{
	return mini_type_get_underlying_type (type);
}

void
mini_jit_init (void)
{
	mono_os_mutex_init_recursive (&jit_mutex);

	mono_counters_register ("Allocated seq points size", MONO_COUNTER_JIT | MONO_COUNTER_INT, &mono_jit_stats.allocated_seq_points_size);
}

void
mini_jit_cleanup (void)
{
	g_free (emul_opcode_map);
	g_free (emul_opcode_opcodes);
}

#if !defined(ENABLE_LLVM_RUNTIME) && !defined(ENABLE_LLVM)

void
mono_llvm_cpp_throw_exception (void)
{
	g_assert_not_reached ();
}

void
mono_llvm_cpp_catch_exception (MonoLLVMInvokeCallback cb, gpointer arg, gboolean *out_thrown)
{
	g_assert_not_reached ();
}

#endif

#ifndef DISABLE_JIT

guint8*
mini_realloc_code_slow (MonoCompile *cfg, int size)
{
	const int EXTRA_CODE_SPACE = 16;

	if (cfg->code_len + size > (cfg->code_size - EXTRA_CODE_SPACE)) {
		while (cfg->code_len + size > (cfg->code_size - EXTRA_CODE_SPACE))
			cfg->code_size = cfg->code_size * 2 + EXTRA_CODE_SPACE;
		cfg->native_code = g_realloc (cfg->native_code, cfg->code_size);
		cfg->stat_code_reallocs++;
	}
	return cfg->native_code + cfg->code_len;
}

#endif /* DISABLE_JIT */

gboolean
mini_class_is_system_array (MonoClass *klass)
{
	return m_class_get_parent (klass) == mono_defaults.array_class;
}

/*
 * mono_target_pagesize:
 *
 *   query pagesize used to determine if an implicit NRE can be used
 */
int
mono_target_pagesize (void)
{
	/* We could query the system's pagesize via mono_pagesize (), however there
	 * are pitfalls: sysconf (3) is called on some posix like systems, and per
	 * POSIX.1-2008 this function doesn't have to be async-safe. Since this
	 * function can be called from a signal handler, we simplify things by
	 * using 4k on all targets. Implicit null-checks with an offset larger than
	 * 4k are _very_ uncommon, so we don't mind emitting an explicit null-check
	 * for those cases.
	 */
	return 4 * 1024;
}

MonoCPUFeatures
mini_get_cpu_features (MonoCompile* cfg)
{
	MonoCPUFeatures features = (MonoCPUFeatures)0;
#if !defined(MONO_CROSS_COMPILE)
	if (!cfg->compile_aot || cfg->use_current_cpu) {
		// detect current CPU features if we are in JIT mode or AOT with use_current_cpu flag.
#if defined(TARGET_AMD64) || defined(TARGET_X86)
		features = mono_arch_get_cpu_features ();
#endif
	}
#endif

#if defined(TARGET_ARM64)
	// All Arm64 devices have this set
	features |= MONO_CPU_ARM64_BASE; 
#endif

	// apply parameters passed via -mattr
	return (features | mono_cpu_features_enabled) & ~mono_cpu_features_disabled;
}
