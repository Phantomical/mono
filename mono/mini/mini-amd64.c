/**
 * \file
 * AMD64 backend for the Mono code generator
 *
 * Based on mini-x86.c.
 *
 * Authors:
 *   Paolo Molaro (lupus@ximian.com)
 *   Dietmar Maurer (dietmar@ximian.com)
 *   Patrik Torstensson
 *   Zoltan Varga (vargaz@gmail.com)
 *   Johan Lorensson (lateralusx.github@gmail.com)
 *
 * (C) 2003 Ximian, Inc.
 * Copyright 2003-2011 Novell, Inc (http://www.novell.com)
 * Copyright 2011 Xamarin, Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "mini.h"
#include <string.h>
#include <math.h>
#include <assert.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <mono/metadata/abi-details.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/profiler-private.h>
#include <mono/metadata/mono-debug.h>
#include <mono/metadata/gc-internals.h>
#include <mono/utils/mono-math.h>
#include <mono/utils/mono-mmap.h>
#include <mono/utils/mono-memory-model.h>
#include <mono/utils/mono-tls.h>
#include <mono/utils/mono-hwcap.h>
#include <mono/utils/mono-threads.h>
#include <mono/utils/unlocked.h>

#include "ir-emit.h"
#include "mini-amd64.h"
#include "cpu-amd64.h"
#include "debugger-agent.h"
#include "mini-gc.h"
#include "mini-runtime.h"
#include "aot-runtime.h"

static GENERATE_TRY_GET_CLASS_WITH_CACHE (math, "System", "Math")


#define IS_IMM32(val) ((((guint64)val) >> 32) == 0)

#define IS_REX(inst) (((inst) >= 0x40) && ((inst) <= 0x4f))

/* Offset between fp and the first argument in the callee */
#define ARGS_OFFSET 16
#define GP_SCRATCH_REG AMD64_R11

/* Max number of bblocks before we bail from using more advanced branch placement code */
#define MAX_BBLOCKS_FOR_BRANCH_OPTS 800

/*
 * AMD64 register usage:
 * - callee saved registers are used for global register allocation
 * - %r11 is used for materializing 64 bit constants in opcodes
 * - the rest is used for local allocation
 */

/*
 * Floating point comparison results:
 *                  ZF PF CF
 * A > B            0  0  0
 * A < B            0  0  1
 * A = B            1  0  0
 * A > B            0  0  0
 * UNORDERED        1  1  1
 */

static gboolean
debug_omit_fp (void)
{
#if 0
	return mono_debug_count ();
#else
	return TRUE;
#endif
}

/*
 * arch-amd64.c carries its own copies of amd64_patch and amd64_use_imm32: both
 * halves emit and patch machine code, and a file-local copy stays inlinable in
 * each of them.
 */
static void
amd64_patch (unsigned char* code, gpointer target)
{
	// NOTE: Sometimes code has just been generated, is not running yet,
	// and has no alignment requirements. Sometimes it could be running while we patch it,
	// and there are alignment requirements.
	// FIXME Assert alignment.

	guint8 rex = 0;

	/* Skip REX */
	if ((code [0] >= 0x40) && (code [0] <= 0x4f)) {
		rex = code [0];
		code += 1;
	}

	if ((code [0] & 0xf8) == 0xb8) {
		/* amd64_set_reg_template */
		*(guint64*)(code + 1) = (guint64)target;
	}
	else if ((code [0] == 0x8b) && rex && x86_modrm_mod (code [1]) == 0 && x86_modrm_rm (code [1]) == 5) {
		/* mov 0(%rip), %dreg */
		g_assert (!1); // Historical code was incorrect.
		ptrdiff_t const offset = (guchar*)target - (code + 6);
		g_assert (offset == (gint32)offset);
		*(gint32*)(code + 2) = (gint32)offset;
	}
	else if (code [0] == 0xff && (code [1] == 0x15 || code [1] == 0x25)) {
		/* call or jmp *<OFFSET>(%rip) */
		// Patch the data, not the code.
		g_assert (!2); // For possible use later.
		*(void**)(code + 6 + *(gint32*)(code + 2)) = target;
	}
	else
		x86_patch (code, target);
}

static gboolean
amd64_use_imm32 (gint64 val)
{
	if (mini_debug_options.single_imm_size)
		return FALSE;

	return amd64_is_imm32 (val);
}

#define DEBUG(a) if (cfg->verbose_level > 1) a

/*
 * mono_arch_get_argument_info:
 * @csig:  a method signature
 * @param_count: the number of parameters to consider
 * @arg_info: an array to store the result infos
 *
 * Gathers information on parameters such as size, alignment and
 * padding. arg_info should be large enought to hold param_count + 1 entries. 
 *
 * Returns the size of the argument area on the stack.
 */
int
mono_arch_get_argument_info (MonoMethodSignature *csig, int param_count, MonoJitArgumentInfo *arg_info)
{
	int k;
	CallInfo *cinfo = mono_arch_get_call_info (NULL, csig);
	guint32 args_size = cinfo->stack_usage;

	/* The arguments are saved to a stack area in mono_arch_instrument_prolog */
	if (csig->hasthis) {
		arg_info [0].offset = 0;
	}

	for (k = 0; k < param_count; k++) {
		arg_info [k + 1].offset = ((k + csig->hasthis) * 8);
		/* FIXME: */
		arg_info [k + 1].size = 0;
	}

	g_free (cinfo);

	return args_size;
}

#ifndef DISABLE_JIT
gboolean
mono_arch_tailcall_supported (MonoCompile *cfg, MonoMethodSignature *caller_sig, MonoMethodSignature *callee_sig, gboolean virtual_)
{
	CallInfo *caller_info = mono_arch_get_call_info (NULL, caller_sig);
	CallInfo *callee_info = mono_arch_get_call_info (NULL, callee_sig);
	gboolean res = IS_SUPPORTED_TAILCALL (callee_info->stack_usage <= caller_info->stack_usage)
                    && IS_SUPPORTED_TAILCALL (callee_info->ret.storage == caller_info->ret.storage);

	// Limit stack_usage to 1G. Assume 32bit limits when we move parameters.
	res &= IS_SUPPORTED_TAILCALL (callee_info->stack_usage < (1 << 30));
	res &= IS_SUPPORTED_TAILCALL (caller_info->stack_usage < (1 << 30));

	// valuetype parameters are address of local
	const ArgInfo *ainfo;
	ainfo = callee_info->args + callee_sig->hasthis;
	for (int i = 0; res && i < callee_sig->param_count; ++i) {
		res = IS_SUPPORTED_TAILCALL (ainfo [i].storage != ArgValuetypeAddrInIReg)
			&& IS_SUPPORTED_TAILCALL (ainfo [i].storage != ArgValuetypeAddrOnStack);
	}

	g_free (caller_info);
	g_free (callee_info);

	return res;
}
#endif /* DISABLE_JIT */

#ifndef DISABLE_JIT

GList *
mono_arch_get_allocatable_int_vars (MonoCompile *cfg)
{
	GList *vars = NULL;
	int i;

	for (i = 0; i < cfg->num_varinfo; i++) {
		MonoInst *ins = cfg->varinfo [i];
		MonoMethodVar *vmv = MONO_VARINFO (cfg, i);

		/* unused vars */
		if (vmv->range.first_use.abs_pos >= vmv->range.last_use.abs_pos)
			continue;

		if ((ins->flags & (MONO_INST_IS_DEAD|MONO_INST_VOLATILE|MONO_INST_INDIRECT)) || 
		    (ins->opcode != OP_LOCAL && ins->opcode != OP_ARG))
			continue;

		if (mono_is_regsize_var (ins->inst_vtype)) {
			g_assert (MONO_VARINFO (cfg, i)->reg == -1);
			g_assert (i == vmv->idx);
			vars = g_list_prepend (vars, vmv);
		}
	}

	vars = mono_varlist_sort (cfg, vars, 0);

	return vars;
}

/**
 * mono_arch_compute_omit_fp:
 * Determine whether the frame pointer can be eliminated.
 */
static void
mono_arch_compute_omit_fp (MonoCompile *cfg)
{
	MonoMethodSignature *sig;
	MonoMethodHeader *header;
	int i, locals_size;
	CallInfo *cinfo;

	if (cfg->arch.omit_fp_computed)
		return;

	header = cfg->header;

	sig = mono_method_signature_internal (cfg->method);

	if (!cfg->arch.cinfo)
		cfg->arch.cinfo = mono_arch_get_call_info (cfg->mempool, sig);
	cinfo = cfg->arch.cinfo;

	/*
	 * FIXME: Remove some of the restrictions.
	 */
	cfg->arch.omit_fp = TRUE;
	cfg->arch.omit_fp_computed = TRUE;

	if (cfg->disable_omit_fp)
		cfg->arch.omit_fp = FALSE;

	if (!debug_omit_fp ())
		cfg->arch.omit_fp = FALSE;
	/*
	if (cfg->method->save_lmf)
		cfg->arch.omit_fp = FALSE;
	*/
	if (cfg->flags & MONO_CFG_HAS_ALLOCA)
		cfg->arch.omit_fp = FALSE;
	if (header->num_clauses)
		cfg->arch.omit_fp = FALSE;
	if (cfg->param_area)
		cfg->arch.omit_fp = FALSE;
	if (!sig->pinvoke && (sig->call_convention == MONO_CALL_VARARG))
		cfg->arch.omit_fp = FALSE;
	for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
		ArgInfo *ainfo = &cinfo->args [i];

		if (ainfo->storage == ArgOnStack || ainfo->storage == ArgValuetypeAddrInIReg || ainfo->storage == ArgValuetypeAddrOnStack) {
			/* 
			 * The stack offset can only be determined when the frame
			 * size is known.
			 */
			cfg->arch.omit_fp = FALSE;
		}
	}

	locals_size = 0;
	for (i = cfg->locals_start; i < cfg->num_varinfo; i++) {
		MonoInst *ins = cfg->varinfo [i];
		int ialign;

		locals_size += mono_type_size (ins->inst_vtype, &ialign);
	}
}

GList *
mono_arch_get_global_int_regs (MonoCompile *cfg)
{
	GList *regs = NULL;

	mono_arch_compute_omit_fp (cfg);

	if (cfg->arch.omit_fp)
		regs = g_list_prepend (regs, (gpointer)AMD64_RBP);

	/* We use the callee saved registers for global allocation */
	regs = g_list_prepend (regs, (gpointer)AMD64_RBX);
	regs = g_list_prepend (regs, (gpointer)AMD64_R12);
	regs = g_list_prepend (regs, (gpointer)AMD64_R13);
	regs = g_list_prepend (regs, (gpointer)AMD64_R14);
	regs = g_list_prepend (regs, (gpointer)AMD64_R15);
#ifdef TARGET_WIN32
	regs = g_list_prepend (regs, (gpointer)AMD64_RDI);
	regs = g_list_prepend (regs, (gpointer)AMD64_RSI);
#endif

	return regs;
}

/*
 * mono_arch_regalloc_cost:
 *
 *  Return the cost, in number of memory references, of the action of 
 * allocating the variable VMV into a register during global register
 * allocation.
 */
guint32
mono_arch_regalloc_cost (MonoCompile *cfg, MonoMethodVar *vmv)
{
	MonoInst *ins = cfg->varinfo [vmv->idx];

	if (cfg->method->save_lmf)
		/* The register is already saved */
		/* substract 1 for the invisible store in the prolog */
		return (ins->opcode == OP_ARG) ? 0 : 1;
	else
		/* push+pop */
		return (ins->opcode == OP_ARG) ? 1 : 2;
}

/*
 * mono_arch_fill_argument_info:
 *
 *   Populate cfg->args, cfg->ret and cfg->vret_addr with information about the arguments
 * of the method.
 */
void
mono_arch_fill_argument_info (MonoCompile *cfg)
{
	MonoMethodSignature *sig;
	MonoInst *ins;
	int i;
	CallInfo *cinfo;

	sig = mono_method_signature_internal (cfg->method);

	cinfo = cfg->arch.cinfo;

	/*
	 * Contrary to mono_arch_allocate_vars (), the information should describe
	 * where the arguments are at the beginning of the method, not where they can be 
	 * accessed during the execution of the method. The later makes no sense for the 
	 * global register allocator, since a variable can be in more than one location.
	 */
	switch (cinfo->ret.storage) {
	case ArgInIReg:
	case ArgInFloatSSEReg:
	case ArgInDoubleSSEReg:
		cfg->ret->opcode = OP_REGVAR;
		cfg->ret->inst_c0 = cinfo->ret.reg;
		break;
	case ArgValuetypeInReg:
		cfg->ret->opcode = OP_REGOFFSET;
		cfg->ret->inst_basereg = -1;
		cfg->ret->inst_offset = -1;
		break;
	case ArgNone:
		break;
	default:
		g_assert_not_reached ();
	}

	for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
		ArgInfo *ainfo = &cinfo->args [i];

		ins = cfg->args [i];

		switch (ainfo->storage) {
		case ArgInIReg:
		case ArgInFloatSSEReg:
		case ArgInDoubleSSEReg:
			ins->opcode = OP_REGVAR;
			ins->inst_c0 = ainfo->reg;
			break;
		case ArgOnStack:
			ins->opcode = OP_REGOFFSET;
			ins->inst_basereg = -1;
			ins->inst_offset = -1;
			break;
		case ArgValuetypeInReg:
			/* Dummy */
			ins->opcode = OP_NOP;
			break;
		default:
			g_assert_not_reached ();
		}
	}
}
 
void
mono_arch_allocate_vars (MonoCompile *cfg)
{
	MonoType *sig_ret;
	MonoMethodSignature *sig;
	MonoInst *ins;
	int i, offset;
	guint32 locals_stack_size, locals_stack_align;
	gint32 *offsets;
	CallInfo *cinfo;

	sig = mono_method_signature_internal (cfg->method);

	cinfo = cfg->arch.cinfo;
	sig_ret = mini_get_underlying_type (sig->ret);

	mono_arch_compute_omit_fp (cfg);

	/*
	 * We use the ABI calling conventions for managed code as well.
	 * Exception: valuetypes are only sometimes passed or returned in registers.
	 */

	/*
	 * The stack looks like this:
	 * <incoming arguments passed on the stack>
	 * <return value>
	 * <lmf/caller saved registers>
	 * <locals>
	 * <spill area>
	 * <localloc area>  -> grows dynamically
	 * <params area>
	 */

	if (cfg->arch.omit_fp) {
		cfg->flags |= MONO_CFG_HAS_SPILLUP;
		cfg->frame_reg = AMD64_RSP;
		offset = 0;
	} else {
		/* Locals are allocated backwards from %fp */
		cfg->frame_reg = AMD64_RBP;
		offset = 0;
	}

	cfg->arch.saved_iregs = cfg->used_int_regs;
	if (cfg->method->save_lmf) {
		/* Save all callee-saved registers normally (except RBP, if not already used), and restore them when unwinding through an LMF */
		guint32 iregs_to_save = AMD64_CALLEE_SAVED_REGS & ~(1<<AMD64_RBP);
		cfg->arch.saved_iregs |= iregs_to_save;
	}

	if (cfg->arch.omit_fp)
		cfg->arch.reg_save_area_offset = offset;
	/* Reserve space for callee saved registers */
	for (i = 0; i < AMD64_NREG; ++i)
		if (AMD64_IS_CALLEE_SAVED_REG (i) && (cfg->arch.saved_iregs & (1 << i))) {
			offset += sizeof (target_mgreg_t);
		}
	if (!cfg->arch.omit_fp)
		cfg->arch.reg_save_area_offset = -offset;

	if (sig_ret->type != MONO_TYPE_VOID) {
		switch (cinfo->ret.storage) {
		case ArgInIReg:
		case ArgInFloatSSEReg:
		case ArgInDoubleSSEReg:
			cfg->ret->opcode = OP_REGVAR;
			cfg->ret->inst_c0 = cinfo->ret.reg;
			cfg->ret->dreg = cinfo->ret.reg;
			break;
		case ArgValuetypeAddrInIReg:
		case ArgGsharedvtVariableInReg:
			/* The register is volatile */
			cfg->vret_addr->opcode = OP_REGOFFSET;
			cfg->vret_addr->inst_basereg = cfg->frame_reg;
			if (cfg->arch.omit_fp) {
				cfg->vret_addr->inst_offset = offset;
				offset += 8;
			} else {
				offset += 8;
				cfg->vret_addr->inst_offset = -offset;
			}
			if (G_UNLIKELY (cfg->verbose_level > 1)) {
				printf ("vret_addr =");
				mono_print_ins (cfg->vret_addr);
			}
			break;
		case ArgValuetypeInReg:
			/* Allocate a local to hold the result, the epilog will copy it to the correct place */
			cfg->ret->opcode = OP_REGOFFSET;
			cfg->ret->inst_basereg = cfg->frame_reg;
			if (cfg->arch.omit_fp) {
				cfg->ret->inst_offset = offset;
				offset += cinfo->ret.pair_storage [1] == ArgNone ? 8 : 16;
			} else {
				offset += cinfo->ret.pair_storage [1] == ArgNone ? 8 : 16;
				cfg->ret->inst_offset = - offset;
			}
			break;
		default:
			g_assert_not_reached ();
		}
	}

	/* Allocate locals */
	offsets = mono_allocate_stack_slots (cfg, cfg->arch.omit_fp ? FALSE: TRUE, &locals_stack_size, &locals_stack_align);
	if (locals_stack_align) {
		offset += (locals_stack_align - 1);
		offset &= ~(locals_stack_align - 1);
	}
	if (cfg->arch.omit_fp) {
		cfg->locals_min_stack_offset = offset;
		cfg->locals_max_stack_offset = offset + locals_stack_size;
	} else {
		cfg->locals_min_stack_offset = - (offset + locals_stack_size);
		cfg->locals_max_stack_offset = - offset;
	}
		
	for (i = cfg->locals_start; i < cfg->num_varinfo; i++) {
		if (offsets [i] != -1) {
			MonoInst *ins = cfg->varinfo [i];
			ins->opcode = OP_REGOFFSET;
			ins->inst_basereg = cfg->frame_reg;
			if (cfg->arch.omit_fp)
				ins->inst_offset = (offset + offsets [i]);
			else
				ins->inst_offset = - (offset + offsets [i]);
			//printf ("allocated local %d to ", i); mono_print_tree_nl (ins);
		}
	}
	offset += locals_stack_size;

	if (!sig->pinvoke && (sig->call_convention == MONO_CALL_VARARG)) {
		g_assert (!cfg->arch.omit_fp);
		g_assert (cinfo->sig_cookie.storage == ArgOnStack);
		cfg->sig_cookie = cinfo->sig_cookie.offset + ARGS_OFFSET;
	}

	for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
		ins = cfg->args [i];
		if (ins->opcode != OP_REGVAR) {
			ArgInfo *ainfo = &cinfo->args [i];
			gboolean inreg = TRUE;

			/* FIXME: Allocate volatile arguments to registers */
			if (ins->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT))
				inreg = FALSE;

			/* 
			 * Under AMD64, all registers used to pass arguments to functions
			 * are volatile across calls.
			 * FIXME: Optimize this.
			 */
			if ((ainfo->storage == ArgInIReg) || (ainfo->storage == ArgInFloatSSEReg) || (ainfo->storage == ArgInDoubleSSEReg) || (ainfo->storage == ArgValuetypeInReg) || (ainfo->storage == ArgGSharedVtInReg))
				inreg = FALSE;

			ins->opcode = OP_REGOFFSET;

			switch (ainfo->storage) {
			case ArgInIReg:
			case ArgInFloatSSEReg:
			case ArgInDoubleSSEReg:
			case ArgGSharedVtInReg:
				if (inreg) {
					ins->opcode = OP_REGVAR;
					ins->dreg = ainfo->reg;
				}
				break;
			case ArgOnStack:
			case ArgGSharedVtOnStack:
				g_assert (!cfg->arch.omit_fp);
				ins->opcode = OP_REGOFFSET;
				ins->inst_basereg = cfg->frame_reg;
				ins->inst_offset = ainfo->offset + ARGS_OFFSET;
				break;
			case ArgValuetypeInReg:
				break;
			case ArgValuetypeAddrInIReg:
			case ArgValuetypeAddrOnStack: {
				MonoInst *indir;
				g_assert (!cfg->arch.omit_fp);
				g_assert (ainfo->storage == ArgValuetypeAddrInIReg || (ainfo->storage == ArgValuetypeAddrOnStack && ainfo->pair_storage [0] == ArgNone));
				MONO_INST_NEW (cfg, indir, 0);

				indir->opcode = OP_REGOFFSET;
				if (ainfo->pair_storage [0] == ArgInIReg) {
					indir->inst_basereg = cfg->frame_reg;
					offset = ALIGN_TO (offset, sizeof (target_mgreg_t));
					offset += sizeof (target_mgreg_t);
					indir->inst_offset = - offset;
				}
				else {
					indir->inst_basereg = cfg->frame_reg;
					indir->inst_offset = ainfo->offset + ARGS_OFFSET;
				}
				
				ins->opcode = OP_VTARG_ADDR;
				ins->inst_left = indir;
				
				break;
			}
			default:
				NOT_IMPLEMENTED;
			}

			if (!inreg && (ainfo->storage != ArgOnStack) && (ainfo->storage != ArgValuetypeAddrInIReg) && (ainfo->storage != ArgValuetypeAddrOnStack) && (ainfo->storage != ArgGSharedVtOnStack)) {
				ins->opcode = OP_REGOFFSET;
				ins->inst_basereg = cfg->frame_reg;
				/* These arguments are saved to the stack in the prolog */
				offset = ALIGN_TO (offset, sizeof (target_mgreg_t));
				if (cfg->arch.omit_fp) {
					ins->inst_offset = offset;
					offset += (ainfo->storage == ArgValuetypeInReg) ? ainfo->nregs * sizeof (target_mgreg_t) : sizeof (target_mgreg_t);
					// Arguments are yet supported by the stack map creation code
					//cfg->locals_max_stack_offset = MAX (cfg->locals_max_stack_offset, offset);
				} else {
					offset += (ainfo->storage == ArgValuetypeInReg) ? ainfo->nregs * sizeof (target_mgreg_t) : sizeof (target_mgreg_t);
					ins->inst_offset = - offset;
					//cfg->locals_min_stack_offset = MIN (cfg->locals_min_stack_offset, offset);
				}
			}
		}
	}

	cfg->stack_offset = offset;
}

void
mono_arch_create_vars (MonoCompile *cfg)
{
	MonoMethodSignature *sig;
	CallInfo *cinfo;

	sig = mono_method_signature_internal (cfg->method);

	if (!cfg->arch.cinfo)
		cfg->arch.cinfo = mono_arch_get_call_info (cfg->mempool, sig);
	cinfo = cfg->arch.cinfo;

	if (cinfo->ret.storage == ArgValuetypeInReg)
		cfg->ret_var_is_local = TRUE;

	if (cinfo->ret.storage == ArgValuetypeAddrInIReg || cinfo->ret.storage == ArgGsharedvtVariableInReg) {
		cfg->vret_addr = mono_compile_create_var (cfg, mono_get_int_type (), OP_ARG);
		if (G_UNLIKELY (cfg->verbose_level > 1)) {
			printf ("vret_addr = ");
			mono_print_ins (cfg->vret_addr);
		}
	}

	if (cfg->gen_sdb_seq_points) {
		MonoInst *ins;

		if (cfg->compile_aot) {
			MonoInst *ins = mono_compile_create_var (cfg, mono_get_int_type (), OP_LOCAL);
			ins->flags |= MONO_INST_VOLATILE;
			cfg->arch.seq_point_info_var = ins;
		}
		ins = mono_compile_create_var (cfg, mono_get_int_type (), OP_LOCAL);
		ins->flags |= MONO_INST_VOLATILE;
		cfg->arch.ss_tramp_var = ins;

		ins = mono_compile_create_var (cfg, mono_get_int_type (), OP_LOCAL);
		ins->flags |= MONO_INST_VOLATILE;
		cfg->arch.bp_tramp_var = ins;
	}

	if (cfg->method->save_lmf)
		cfg->create_lmf_var = TRUE;

	if (cfg->method->save_lmf) {
		cfg->lmf_ir = TRUE;
	}
}

static void
add_outarg_reg (MonoCompile *cfg, MonoCallInst *call, ArgStorage storage, int reg, MonoInst *tree)
{
	MonoInst *ins;

	switch (storage) {
	case ArgInIReg:
		MONO_INST_NEW (cfg, ins, OP_MOVE);
		ins->dreg = mono_alloc_ireg_copy (cfg, tree->dreg);
		ins->sreg1 = tree->dreg;
		MONO_ADD_INS (cfg->cbb, ins);
		mono_call_inst_add_outarg_reg (cfg, call, ins->dreg, reg, FALSE);
		break;
	case ArgInFloatSSEReg:
		MONO_INST_NEW (cfg, ins, OP_AMD64_SET_XMMREG_R4);
		ins->dreg = mono_alloc_freg (cfg);
		ins->sreg1 = tree->dreg;
		MONO_ADD_INS (cfg->cbb, ins);

		mono_call_inst_add_outarg_reg (cfg, call, ins->dreg, reg, TRUE);
		break;
	case ArgInDoubleSSEReg:
		MONO_INST_NEW (cfg, ins, OP_FMOVE);
		ins->dreg = mono_alloc_freg (cfg);
		ins->sreg1 = tree->dreg;
		MONO_ADD_INS (cfg->cbb, ins);

		mono_call_inst_add_outarg_reg (cfg, call, ins->dreg, reg, TRUE);

		break;
	default:
		g_assert_not_reached ();
	}
}

static int
arg_storage_to_load_membase (ArgStorage storage)
{
	switch (storage) {
	case ArgInIReg:
#if defined(MONO_ARCH_ILP32)
		return OP_LOADI8_MEMBASE;
#else
		return OP_LOAD_MEMBASE;
#endif
	case ArgInDoubleSSEReg:
		return OP_LOADR8_MEMBASE;
	case ArgInFloatSSEReg:
		return OP_LOADR4_MEMBASE;
	default:
		g_assert_not_reached ();
	}

	return -1;
}

static void
emit_sig_cookie (MonoCompile *cfg, MonoCallInst *call, CallInfo *cinfo)
{
	MonoMethodSignature *tmp_sig;
	int sig_reg;

	if (call->tailcall) // FIXME tailcall is not always yet initialized.
		NOT_IMPLEMENTED;

	g_assert (cinfo->sig_cookie.storage == ArgOnStack);
			
	/*
	 * mono_ArgIterator_Setup assumes the signature cookie is 
	 * passed first and all the arguments which were before it are
	 * passed on the stack after the signature. So compensate by 
	 * passing a different signature.
	 */
	tmp_sig = mono_metadata_signature_dup_full (m_class_get_image (cfg->method->klass), call->signature);
	tmp_sig->param_count -= call->signature->sentinelpos;
	tmp_sig->sentinelpos = 0;
	memcpy (tmp_sig->params, call->signature->params + call->signature->sentinelpos, tmp_sig->param_count * sizeof (MonoType*));

	sig_reg = mono_alloc_ireg (cfg);
	MONO_EMIT_NEW_SIGNATURECONST (cfg, sig_reg, tmp_sig);

	MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORE_MEMBASE_REG, AMD64_RSP, cinfo->sig_cookie.offset, sig_reg);
}

#ifdef ENABLE_LLVM
static LLVMArgStorage
arg_storage_to_llvm_arg_storage (MonoCompile *cfg, ArgStorage storage)
{
	switch (storage) {
	case ArgInIReg:
		return LLVMArgInIReg;
	case ArgNone:
		return LLVMArgNone;
	case ArgGSharedVtInReg:
	case ArgGSharedVtOnStack:
		return LLVMArgGSharedVt;
	default:
		g_assert_not_reached ();
		return LLVMArgNone;
	}
}

LLVMCallInfo*
mono_arch_get_llvm_call_info (MonoCompile *cfg, MonoMethodSignature *sig)
{
	int i, n;
	CallInfo *cinfo;
	ArgInfo *ainfo;
	int j;
	LLVMCallInfo *linfo;
	MonoType *t, *sig_ret;

	n = sig->param_count + sig->hasthis;
	sig_ret = mini_get_underlying_type (sig->ret);

	cinfo = mono_arch_get_call_info (cfg->mempool, sig);

	linfo = mono_mempool_alloc0 (cfg->mempool, sizeof (LLVMCallInfo) + (sizeof (LLVMArgInfo) * n));

	/*
	 * LLVM always uses the native ABI while we use our own ABI, the
	 * only difference is the handling of vtypes:
	 * - we only pass/receive them in registers in some cases, and only 
	 *   in 1 or 2 integer registers.
	 */
	switch (cinfo->ret.storage) {
	case ArgNone:
		linfo->ret.storage = LLVMArgNone;
		break;
	case ArgInIReg:
	case ArgInFloatSSEReg:
	case ArgInDoubleSSEReg:
		linfo->ret.storage = LLVMArgNormal;
		break;
	case ArgValuetypeInReg: {
		ainfo = &cinfo->ret;

		if (sig->pinvoke &&
			(ainfo->pair_storage [0] == ArgInFloatSSEReg || ainfo->pair_storage [0] == ArgInDoubleSSEReg ||
			 ainfo->pair_storage [1] == ArgInFloatSSEReg || ainfo->pair_storage [1] == ArgInDoubleSSEReg)) {
			cfg->exception_message = g_strdup ("pinvoke + vtype ret");
			cfg->disable_llvm = TRUE;
			return linfo;
		}

		linfo->ret.storage = LLVMArgVtypeInReg;
		for (j = 0; j < 2; ++j)
			linfo->ret.pair_storage [j] = arg_storage_to_llvm_arg_storage (cfg, ainfo->pair_storage [j]);
		break;
	}
	case ArgValuetypeAddrInIReg:
	case ArgGsharedvtVariableInReg:
		/* Vtype returned using a hidden argument */
		linfo->ret.storage = LLVMArgVtypeRetAddr;
		linfo->vret_arg_index = cinfo->vret_arg_index;
		break;
	default:
		g_assert_not_reached ();
		break;
	}

	for (i = 0; i < n; ++i) {
		ainfo = cinfo->args + i;

		if (i >= sig->hasthis)
			t = sig->params [i - sig->hasthis];
		else
			t = mono_get_int_type ();
		t = mini_type_get_underlying_type (t);

		linfo->args [i].storage = LLVMArgNone;

		switch (ainfo->storage) {
		case ArgInIReg:
			linfo->args [i].storage = LLVMArgNormal;
			break;
		case ArgInDoubleSSEReg:
		case ArgInFloatSSEReg:
			linfo->args [i].storage = LLVMArgNormal;
			break;
		case ArgOnStack:
			if (MONO_TYPE_ISSTRUCT (t))
				linfo->args [i].storage = LLVMArgVtypeByVal;
			else
				linfo->args [i].storage = LLVMArgNormal;
			break;
		case ArgValuetypeInReg:
			if (sig->pinvoke &&
				(ainfo->pair_storage [0] == ArgInFloatSSEReg || ainfo->pair_storage [0] == ArgInDoubleSSEReg ||
				 ainfo->pair_storage [1] == ArgInFloatSSEReg || ainfo->pair_storage [1] == ArgInDoubleSSEReg)) {
				cfg->exception_message = g_strdup ("pinvoke + vtypes");
				cfg->disable_llvm = TRUE;
				return linfo;
			}

			linfo->args [i].storage = LLVMArgVtypeInReg;
			for (j = 0; j < 2; ++j)
				linfo->args [i].pair_storage [j] = arg_storage_to_llvm_arg_storage (cfg, ainfo->pair_storage [j]);
			break;
		case ArgGSharedVtInReg:
		case ArgGSharedVtOnStack:
			linfo->args [i].storage = LLVMArgGSharedVt;
			break;
		case ArgValuetypeAddrInIReg:
		case ArgValuetypeAddrOnStack:
			linfo->args [i].storage = LLVMArgVtypeAddr;
			break;
		default:
			cfg->exception_message = g_strdup ("ainfo->storage");
			cfg->disable_llvm = TRUE;
			break;
		}
	}

	return linfo;
}
#endif

void
mono_arch_emit_call (MonoCompile *cfg, MonoCallInst *call)
{
	MonoInst *arg, *in;
	MonoMethodSignature *sig;
	int i, n;
	CallInfo *cinfo;
	ArgInfo *ainfo;

	sig = call->signature;
	n = sig->param_count + sig->hasthis;

	cinfo = mono_arch_get_call_info (cfg->mempool, sig);

	if (COMPILE_LLVM (cfg)) {
		/* We shouldn't be called in the llvm case */
		cfg->disable_llvm = TRUE;
		return;
	}

	/* 
	 * Emit all arguments which are passed on the stack to prevent register
	 * allocation problems.
	 */
	for (i = 0; i < n; ++i) {
		MonoType *t;
		ainfo = cinfo->args + i;

		in = call->args [i];

		if (sig->hasthis && i == 0)
			t = mono_get_object_type ();
		else
			t = sig->params [i - sig->hasthis];

		t = mini_get_underlying_type (t);
		//XXX what about ArgGSharedVtOnStack here?
		if (ainfo->storage == ArgOnStack && !MONO_TYPE_ISSTRUCT (t)) {
			if (!t->byref) {
				if (t->type == MONO_TYPE_R4)
					MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORER4_MEMBASE_REG, AMD64_RSP, ainfo->offset, in->dreg);
				else if (t->type == MONO_TYPE_R8)
					MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORER8_MEMBASE_REG, AMD64_RSP, ainfo->offset, in->dreg);
				else
					MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORE_MEMBASE_REG, AMD64_RSP, ainfo->offset, in->dreg);
			} else {
				MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORE_MEMBASE_REG, AMD64_RSP, ainfo->offset, in->dreg);
			}
			if (cfg->compute_gc_maps) {
				MonoInst *def;

				EMIT_NEW_GC_PARAM_SLOT_LIVENESS_DEF (cfg, def, ainfo->offset, t);
			}
		}
	}

	/*
	 * Emit all parameters passed in registers in non-reverse order for better readability
	 * and to help the optimization in emit_prolog ().
	 */
	for (i = 0; i < n; ++i) {
		ainfo = cinfo->args + i;

		in = call->args [i];

		if (ainfo->storage == ArgInIReg)
			add_outarg_reg (cfg, call, ainfo->storage, ainfo->reg, in);
	}

	for (i = n - 1; i >= 0; --i) {
		MonoType *t;

		ainfo = cinfo->args + i;

		in = call->args [i];

		if (sig->hasthis && i == 0)
			t = mono_get_object_type ();
		else
			t = sig->params [i - sig->hasthis];
		t = mini_get_underlying_type (t);

		switch (ainfo->storage) {
		case ArgInIReg:
			/* Already done */
			break;
		case ArgInFloatSSEReg:
		case ArgInDoubleSSEReg:
			add_outarg_reg (cfg, call, ainfo->storage, ainfo->reg, in);
			break;
		case ArgOnStack:
		case ArgValuetypeInReg:
		case ArgValuetypeAddrInIReg:
		case ArgValuetypeAddrOnStack:
		case ArgGSharedVtInReg:
		case ArgGSharedVtOnStack: {
			if (ainfo->storage == ArgOnStack && !MONO_TYPE_ISSTRUCT (t))
				/* Already emitted above */
				break;

			guint32 align;
			guint32 size;

			if (sig->pinvoke)
				size = mono_type_native_stack_size (t, &align);
			else {
				/*
				 * Other backends use mono_type_stack_size (), but that
				 * aligns the size to 8, which is larger than the size of
				 * the source, leading to reads of invalid memory if the
				 * source is at the end of address space.
				 */
				size = mono_class_value_size (mono_class_from_mono_type_internal (t), &align);
			}

			if (size >= 10000) {
				/* Avoid asserts in emit_memcpy () */
				mono_cfg_set_exception_invalid_program (cfg, g_strdup_printf ("Passing an argument of size '%d'.", size));
				/* Continue normally */
			}

			if (size > 0 || ainfo->pass_empty_struct) {
				MONO_INST_NEW (cfg, arg, OP_OUTARG_VT);
				arg->sreg1 = in->dreg;
				arg->klass = mono_class_from_mono_type_internal (t);
				arg->backend.size = size;
				arg->inst_p0 = call;
				arg->inst_p1 = mono_mempool_alloc (cfg->mempool, sizeof (ArgInfo));
				memcpy (arg->inst_p1, ainfo, sizeof (ArgInfo));

				MONO_ADD_INS (cfg->cbb, arg);
			}
			break;
		}
		default:
			g_assert_not_reached ();
		}

		if (!sig->pinvoke && (sig->call_convention == MONO_CALL_VARARG) && (i == sig->sentinelpos))
			/* Emit the signature cookie just before the implicit arguments */
			emit_sig_cookie (cfg, call, cinfo);
	}

	/* Handle the case where there are no implicit arguments */
	if (!sig->pinvoke && (sig->call_convention == MONO_CALL_VARARG) && (n == sig->sentinelpos))
		emit_sig_cookie (cfg, call, cinfo);

	switch (cinfo->ret.storage) {
	case ArgValuetypeInReg:
		if (cinfo->ret.pair_storage [0] == ArgInIReg && cinfo->ret.pair_storage [1] == ArgNone) {
			/*
			 * Tell the JIT to use a more efficient calling convention: call using
			 * OP_CALL, compute the result location after the call, and save the
			 * result there.
			 */
			call->vret_in_reg = TRUE;
			/*
			 * Nullify the instruction computing the vret addr to enable
			 * future optimizations.
			 */
			if (call->vret_var)
				NULLIFY_INS (call->vret_var);
		} else {
			if (call->tailcall)
				NOT_IMPLEMENTED;
			/*
			 * The valuetype is in RAX:RDX after the call, need to be copied to
			 * the stack. Push the address here, so the call instruction can
			 * access it.
			 */
			if (!cfg->arch.vret_addr_loc) {
				cfg->arch.vret_addr_loc = mono_compile_create_var (cfg, mono_get_int_type (), OP_LOCAL);
				/* Prevent it from being register allocated or optimized away */
				cfg->arch.vret_addr_loc->flags |= MONO_INST_VOLATILE;
			}

			MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, cfg->arch.vret_addr_loc->dreg, call->vret_var->dreg);
		}
		break;
	case ArgValuetypeAddrInIReg:
	case ArgGsharedvtVariableInReg: {
		MonoInst *vtarg;
		MONO_INST_NEW (cfg, vtarg, OP_MOVE);
		vtarg->sreg1 = call->vret_var->dreg;
		vtarg->dreg = mono_alloc_preg (cfg);
		MONO_ADD_INS (cfg->cbb, vtarg);

		mono_call_inst_add_outarg_reg (cfg, call, vtarg->dreg, cinfo->ret.reg, FALSE);
		break;
	}
	default:
		break;
	}

	if (cfg->method->save_lmf) {
		MONO_INST_NEW (cfg, arg, OP_AMD64_SAVE_SP_TO_LMF);
		MONO_ADD_INS (cfg->cbb, arg);
	}

	call->stack_usage = cinfo->stack_usage;
}

void
mono_arch_emit_outarg_vt (MonoCompile *cfg, MonoInst *ins, MonoInst *src)
{
	MonoInst *arg;
	MonoCallInst *call = (MonoCallInst*)ins->inst_p0;
	ArgInfo *ainfo = (ArgInfo*)ins->inst_p1;
	int size = ins->backend.size;

	switch (ainfo->storage) {
	case ArgValuetypeInReg: {
		MonoInst *load;
		int part;

		for (part = 0; part < 2; ++part) {
			if (ainfo->pair_storage [part] == ArgNone)
				continue;

			if (ainfo->pass_empty_struct) {
				//Pass empty struct value as 0 on platforms representing empty structs as 1 byte.
				NEW_ICONST (cfg, load, 0);
			}
			else {
				MONO_INST_NEW (cfg, load, arg_storage_to_load_membase (ainfo->pair_storage [part]));
				load->inst_basereg = src->dreg;
				load->inst_offset = part * sizeof (target_mgreg_t);

				switch (ainfo->pair_storage [part]) {
				case ArgInIReg:
					load->dreg = mono_alloc_ireg (cfg);
					break;
				case ArgInDoubleSSEReg:
				case ArgInFloatSSEReg:
					load->dreg = mono_alloc_freg (cfg);
					break;
				default:
					g_assert_not_reached ();
				}
			}

			MONO_ADD_INS (cfg->cbb, load);

			add_outarg_reg (cfg, call, ainfo->pair_storage [part], ainfo->pair_regs [part], load);
		}
		break;
	}
	case ArgValuetypeAddrInIReg:
	case ArgValuetypeAddrOnStack: {
		MonoInst *vtaddr, *load;

		g_assert (ainfo->storage == ArgValuetypeAddrInIReg || (ainfo->storage == ArgValuetypeAddrOnStack && ainfo->pair_storage [0] == ArgNone));
		
		vtaddr = mono_compile_create_var (cfg, m_class_get_byval_arg (ins->klass), OP_LOCAL);
		vtaddr->backend.is_pinvoke = call->signature->pinvoke;

		MONO_INST_NEW (cfg, load, OP_LDADDR);
		cfg->has_indirection = TRUE;
		load->inst_p0 = vtaddr;
		vtaddr->flags |= MONO_INST_INDIRECT;
		load->type = STACK_MP;
		load->klass = vtaddr->klass;
		load->dreg = mono_alloc_ireg (cfg);
		MONO_ADD_INS (cfg->cbb, load);
		mini_emit_memcpy (cfg, load->dreg, 0, src->dreg, 0, size, TARGET_SIZEOF_VOID_P);

		if (ainfo->pair_storage [0] == ArgInIReg) {
			MONO_INST_NEW (cfg, arg, OP_AMD64_LEA_MEMBASE);
			arg->dreg = mono_alloc_ireg (cfg);
			arg->sreg1 = load->dreg;
			arg->inst_imm = 0;
			MONO_ADD_INS (cfg->cbb, arg);
			mono_call_inst_add_outarg_reg (cfg, call, arg->dreg, ainfo->pair_regs [0], FALSE);
		} else {
			MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORE_MEMBASE_REG, AMD64_RSP, ainfo->offset, load->dreg);
		}
		break;
	}
	case ArgGSharedVtInReg:
		/* Pass by addr */
		mono_call_inst_add_outarg_reg (cfg, call, src->dreg, ainfo->reg, FALSE);
		break;
	case ArgGSharedVtOnStack:
		MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORE_MEMBASE_REG, AMD64_RSP, ainfo->offset, src->dreg);
		break;
	default:
		if (size == 8) {
			int dreg = mono_alloc_ireg (cfg);

			MONO_EMIT_NEW_LOAD_MEMBASE (cfg, dreg, src->dreg, 0);
			MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORE_MEMBASE_REG, AMD64_RSP, ainfo->offset, dreg);
		} else if (size <= 40) {
			mini_emit_memcpy (cfg, AMD64_RSP, ainfo->offset, src->dreg, 0, size, TARGET_SIZEOF_VOID_P);
		} else {
			// FIXME: Code growth
			mini_emit_memcpy (cfg, AMD64_RSP, ainfo->offset, src->dreg, 0, size, TARGET_SIZEOF_VOID_P);
		}

		if (cfg->compute_gc_maps) {
			MonoInst *def;
			EMIT_NEW_GC_PARAM_SLOT_LIVENESS_DEF (cfg, def, ainfo->offset, m_class_get_byval_arg (ins->klass));
		}
	}
}

void
mono_arch_emit_setret (MonoCompile *cfg, MonoMethod *method, MonoInst *val)
{
	MonoType *ret = mini_get_underlying_type (mono_method_signature_internal (method)->ret);

	if (ret->type == MONO_TYPE_R4) {
		if (COMPILE_LLVM (cfg))
			MONO_EMIT_NEW_UNALU (cfg, OP_FMOVE, cfg->ret->dreg, val->dreg);
		else
			MONO_EMIT_NEW_UNALU (cfg, OP_AMD64_SET_XMMREG_R4, cfg->ret->dreg, val->dreg);
		return;
	} else if (ret->type == MONO_TYPE_R8) {
		MONO_EMIT_NEW_UNALU (cfg, OP_FMOVE, cfg->ret->dreg, val->dreg);
		return;
	}
			
	MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, cfg->ret->dreg, val->dreg);
}

#endif /* DISABLE_JIT */

#define EMIT_COND_BRANCH(ins,cond,sign) \
        if (ins->inst_true_bb->native_offset) { \
	        x86_branch (code, cond, cfg->native_code + ins->inst_true_bb->native_offset, sign); \
        } else { \
	        mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_BB, ins->inst_true_bb); \
	        if (optimize_branch_pred && \
            x86_is_imm8 (ins->inst_true_bb->max_offset - offset)) \
		        x86_branch8 (code, cond, 0, sign); \
                else \
	                x86_branch32 (code, cond, 0, sign); \
}

/* emit an exception if condition is fail */
#define EMIT_COND_SYSTEM_EXCEPTION(cond,signed,exc_name)            \
        do {                                                        \
		MonoInst *tins = mono_branch_optimize_exception_target (cfg, bb, exc_name); \
		if (tins == NULL) {										\
			mono_add_patch_info (cfg, code - cfg->native_code,   \
					MONO_PATCH_INFO_EXC, exc_name);  \
			x86_branch32 (code, cond, 0, signed);               \
		} else {	\
			EMIT_COND_BRANCH (tins, cond, signed);	\
		}			\
	} while (0); 

#define EMIT_SSE2_FPFUNC(code, op, dreg, sreg1) do { \
    amd64_movsd_membase_reg (code, AMD64_RSP, -8, (sreg1)); \
	amd64_fld_membase (code, AMD64_RSP, -8, TRUE); \
	amd64_ ##op (code); \
	amd64_fst_membase (code, AMD64_RSP, -8, TRUE, TRUE); \
	amd64_movsd_reg_membase (code, (dreg), AMD64_RSP, -8); \
} while (0);

#ifndef DISABLE_JIT
static guint8*
emit_call (MonoCompile *cfg, MonoCallInst *call, guint8 *code, MonoJitICallId jit_icall_id)
{
	gboolean no_patch = FALSE;
	MonoJumpInfoTarget patch;

	// FIXME? This is similar to mono_call_to_patch, except it favors MONO_PATCH_INFO_ABS over call->jit_icall_id.

	if (jit_icall_id) {
		g_assert (!call);
		patch.type = MONO_PATCH_INFO_JIT_ICALL_ID;
		patch.target = GUINT_TO_POINTER (jit_icall_id);
	} else if (call->inst.flags & MONO_INST_HAS_METHOD) {
		patch.type = MONO_PATCH_INFO_METHOD;
		patch.target = call->method;
	} else {
		patch.type = MONO_PATCH_INFO_ABS;
		patch.target = call->fptr;
	}

	/* 
	 * FIXME: Add support for thunks
	 */
	{
		gboolean near_call = FALSE;

		/*
		 * Indirect calls are expensive so try to make a near call if possible.
		 * The caller memory is allocated by the code manager so it is 
		 * guaranteed to be at a 32 bit offset.
		 */

		if (patch.type != MONO_PATCH_INFO_ABS) {

			/* The target is in memory allocated using the code manager */
			near_call = TRUE;

			if (patch.type == MONO_PATCH_INFO_METHOD) {

				MonoMethod* const method = call->method;

				if (m_class_get_image (method->klass)->aot_module)
					/* The callee might be an AOT method */
					near_call = FALSE;
				if (method->dynamic)
					/* The target is in malloc-ed memory */
					near_call = FALSE;
			} else {
				/* 
				 * The call might go directly to a native function without
				 * the wrapper.
				 */
				MonoJitICallInfo * const mi = mono_find_jit_icall_info (jit_icall_id);
				gconstpointer target = mono_icall_get_wrapper (mi);
				if ((((guint64)target) >> 32) != 0)
					near_call = FALSE;
			}
		} else {
			MonoJumpInfo *jinfo = NULL;

			if (cfg->abs_patches)
				jinfo = (MonoJumpInfo *)g_hash_table_lookup (cfg->abs_patches, call->fptr);

			if (jinfo) {
				if (jinfo->type == MONO_PATCH_INFO_JIT_ICALL_ADDR) {
					MonoJitICallInfo *mi = mono_find_jit_icall_info (jinfo->data.jit_icall_id);
					if (mi && (((guint64)mi->func) >> 32) == 0)
						near_call = TRUE;
					no_patch = TRUE;
				} else {
					/* 
					 * This is not really an optimization, but required because the
					 * generic class init trampolines use R11 to pass the vtable.
					 */
					near_call = TRUE;
				}
			} else {
				jit_icall_id = call->jit_icall_id;

				if (jit_icall_id) {
					MonoJitICallInfo const *info = mono_find_jit_icall_info (jit_icall_id);

					// Change patch from MONO_PATCH_INFO_ABS to MONO_PATCH_INFO_JIT_ICALL_ID.
					patch.type = MONO_PATCH_INFO_JIT_ICALL_ID;
					patch.target = GUINT_TO_POINTER (jit_icall_id);

					if (info->func == info->wrapper) {
						/* No wrapper */
						if ((((guint64)info->func) >> 32) == 0)
							near_call = TRUE;
					} else {
						/* ?See the comment in mono_codegen ()? */
						near_call = TRUE;
					}
				}
				else if ((((guint64)patch.target) >> 32) == 0) {
					near_call = TRUE;
					no_patch = TRUE;
				}
			}
		}

		if (cfg->method->dynamic)
			/* These methods are allocated using malloc */
			near_call = FALSE;

#ifdef MONO_ARCH_NOMAP32BIT
		near_call = FALSE;
#endif
		/* The 64bit XEN kernel does not honour the MAP_32BIT flag. (#522894) */
		if (mono_amd64_optimize_for_xen)
			near_call = FALSE;

		if (cfg->compile_aot) {
			near_call = TRUE;
			no_patch = TRUE;
		}

		if (near_call) {
			/* 
			 * Align the call displacement to an address divisible by 4 so it does
			 * not span cache lines. This is required for code patching to work on SMP
			 * systems.
			 */
			if (!no_patch && ((guint32)(code + 1 - cfg->native_code) % 4) != 0) {
				guint32 pad_size = 4 - ((guint32)(code + 1 - cfg->native_code) % 4);
				amd64_padding (code, pad_size);
			}
			mono_add_patch_info (cfg, code - cfg->native_code, patch.type, patch.target);
			amd64_call_code (code, 0);
		}
		else {
			if (!no_patch && ((guint32)(code + 2 - cfg->native_code) % 8) != 0) {
				guint32 pad_size = 8 - ((guint32)(code + 2 - cfg->native_code) % 8);
				amd64_padding (code, pad_size);
				g_assert ((guint64)(code + 2 - cfg->native_code) % 8 == 0);
			}
			mono_add_patch_info (cfg, code - cfg->native_code, patch.type, patch.target);
			amd64_set_reg_template (code, GP_SCRATCH_REG);
			amd64_call_reg (code, GP_SCRATCH_REG);
		}
	}

	set_code_cursor (cfg, code);

	return code;
}

static int
store_membase_imm_to_store_membase_reg (int opcode)
{
	switch (opcode) {
	case OP_STORE_MEMBASE_IMM:
		return OP_STORE_MEMBASE_REG;
	case OP_STOREI4_MEMBASE_IMM:
		return OP_STOREI4_MEMBASE_REG;
	case OP_STOREI8_MEMBASE_IMM:
		return OP_STOREI8_MEMBASE_REG;
	}

	return -1;
}


#define INST_IGNORES_CFLAGS(opcode) (!(((opcode) == OP_ADC) || ((opcode) == OP_ADC_IMM) || ((opcode) == OP_IADC) || ((opcode) == OP_IADC_IMM) || ((opcode) == OP_SBB) || ((opcode) == OP_SBB_IMM) || ((opcode) == OP_ISBB) || ((opcode) == OP_ISBB_IMM)))

/*
 * mono_arch_peephole_pass_1:
 *
 *   Perform peephole opts which should/can be performed before local regalloc
 */
void
mono_arch_peephole_pass_1 (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins, *n;

	MONO_BB_FOR_EACH_INS_SAFE (bb, n, ins) {
		MonoInst *last_ins = mono_inst_prev (ins, FILTER_IL_SEQ_POINT);

		switch (ins->opcode) {
		case OP_ADD_IMM:
		case OP_IADD_IMM:
		case OP_LADD_IMM:
			if ((ins->sreg1 < MONO_MAX_IREGS) && (ins->dreg >= MONO_MAX_IREGS) && (ins->inst_imm > 0)) {
				/* 
				 * X86_LEA is like ADD, but doesn't have the
				 * sreg1==dreg restriction. inst_imm > 0 is needed since LEA sign-extends 
				 * its operand to 64 bit.
				 */
				ins->opcode = ins->opcode == OP_IADD_IMM ? OP_X86_LEA_MEMBASE : OP_AMD64_LEA_MEMBASE;
				ins->inst_basereg = ins->sreg1;
			}
			break;
		case OP_LXOR:
		case OP_IXOR:
			if ((ins->sreg1 == ins->sreg2) && (ins->sreg1 == ins->dreg)) {
				MonoInst *ins2;

				/* 
				 * Replace STORE_MEMBASE_IMM 0 with STORE_MEMBASE_REG since 
				 * the latter has length 2-3 instead of 6 (reverse constant
				 * propagation). These instruction sequences are very common
				 * in the initlocals bblock.
				 */
				for (ins2 = ins->next; ins2; ins2 = ins2->next) {
					if (((ins2->opcode == OP_STORE_MEMBASE_IMM) || (ins2->opcode == OP_STOREI4_MEMBASE_IMM) || (ins2->opcode == OP_STOREI8_MEMBASE_IMM) || (ins2->opcode == OP_STORE_MEMBASE_IMM)) && (ins2->inst_imm == 0)) {
						ins2->opcode = store_membase_imm_to_store_membase_reg (ins2->opcode);
						ins2->sreg1 = ins->dreg;
					} else if ((ins2->opcode == OP_STOREI1_MEMBASE_IMM) || (ins2->opcode == OP_STOREI2_MEMBASE_IMM) || (ins2->opcode == OP_STOREI8_MEMBASE_REG) || (ins2->opcode == OP_STORE_MEMBASE_REG)) {
						/* Continue */
					} else if (((ins2->opcode == OP_ICONST) || (ins2->opcode == OP_I8CONST)) && (ins2->dreg == ins->dreg) && (ins2->inst_c0 == 0)) {
						NULLIFY_INS (ins2);
						/* Continue */
					} else if (ins2->opcode == OP_IL_SEQ_POINT) {
						/* Continue */
					} else {
						break;
					}
				}
			}
			break;
		case OP_COMPARE_IMM:
		case OP_LCOMPARE_IMM:
			/* OP_COMPARE_IMM (reg, 0) 
			 * --> 
			 * OP_AMD64_TEST_NULL (reg) 
			 */
			if (!ins->inst_imm)
				ins->opcode = OP_AMD64_TEST_NULL;
			break;
		case OP_ICOMPARE_IMM:
			if (!ins->inst_imm)
				ins->opcode = OP_X86_TEST_NULL;
			break;
		case OP_AMD64_ICOMPARE_MEMBASE_IMM:
			/* 
			 * OP_STORE_MEMBASE_REG reg, offset(basereg)
			 * OP_X86_COMPARE_MEMBASE_IMM offset(basereg), imm
			 * -->
			 * OP_STORE_MEMBASE_REG reg, offset(basereg)
			 * OP_COMPARE_IMM reg, imm
			 *
			 * Note: if imm = 0 then OP_COMPARE_IMM replaced with OP_X86_TEST_NULL
			 */
			if (last_ins && (last_ins->opcode == OP_STOREI4_MEMBASE_REG) &&
			    ins->inst_basereg == last_ins->inst_destbasereg &&
			    ins->inst_offset == last_ins->inst_offset) {
					ins->opcode = OP_ICOMPARE_IMM;
					ins->sreg1 = last_ins->sreg1;

					/* check if we can remove cmp reg,0 with test null */
					if (!ins->inst_imm)
						ins->opcode = OP_X86_TEST_NULL;
				}

			break;
		}

		mono_peephole_ins (bb, ins);
	}
}

void
mono_arch_peephole_pass_2 (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins, *n;

	MONO_BB_FOR_EACH_INS_SAFE (bb, n, ins) {
		switch (ins->opcode) {
		case OP_ICONST:
		case OP_I8CONST: {
			MonoInst *next = mono_inst_next (ins, FILTER_IL_SEQ_POINT);
			/* reg = 0 -> XOR (reg, reg) */
			/* XOR sets cflags on x86, so we cant do it always */
			if (ins->inst_c0 == 0 && (!next || (next && INST_IGNORES_CFLAGS (next->opcode)))) {
				ins->opcode = OP_LXOR;
				ins->sreg1 = ins->dreg;
				ins->sreg2 = ins->dreg;
				/* Fall through */
			} else {
				break;
			}
		}
		case OP_LXOR:
			/*
			 * Use IXOR to avoid a rex prefix if possible. The cpu will sign extend the 
			 * 0 result into 64 bits.
			 */
			if ((ins->sreg1 == ins->sreg2) && (ins->sreg1 == ins->dreg)) {
				ins->opcode = OP_IXOR;
			}
			/* Fall through */
		case OP_IXOR:
			if ((ins->sreg1 == ins->sreg2) && (ins->sreg1 == ins->dreg)) {
				MonoInst *ins2;

				/* 
				 * Replace STORE_MEMBASE_IMM 0 with STORE_MEMBASE_REG since 
				 * the latter has length 2-3 instead of 6 (reverse constant
				 * propagation). These instruction sequences are very common
				 * in the initlocals bblock.
				 */
				for (ins2 = ins->next; ins2; ins2 = ins2->next) {
					if (((ins2->opcode == OP_STORE_MEMBASE_IMM) || (ins2->opcode == OP_STOREI4_MEMBASE_IMM) || (ins2->opcode == OP_STOREI8_MEMBASE_IMM) || (ins2->opcode == OP_STORE_MEMBASE_IMM)) && (ins2->inst_imm == 0)) {
						ins2->opcode = store_membase_imm_to_store_membase_reg (ins2->opcode);
						ins2->sreg1 = ins->dreg;
					} else if ((ins2->opcode == OP_STOREI1_MEMBASE_IMM) || (ins2->opcode == OP_STOREI2_MEMBASE_IMM) || (ins2->opcode == OP_STOREI4_MEMBASE_REG) || (ins2->opcode == OP_STOREI8_MEMBASE_REG) || (ins2->opcode == OP_STORE_MEMBASE_REG) || (ins2->opcode == OP_LIVERANGE_START) || (ins2->opcode == OP_GC_LIVENESS_DEF) || (ins2->opcode == OP_GC_LIVENESS_USE)) {
						/* Continue */
					} else if (((ins2->opcode == OP_ICONST) || (ins2->opcode == OP_I8CONST)) && (ins2->dreg == ins->dreg) && (ins2->inst_c0 == 0)) {
						NULLIFY_INS (ins2);
						/* Continue */
					} else if (ins2->opcode == OP_IL_SEQ_POINT) {
						/* Continue */
					} else {
						break;
					}
				}
			}
			break;
		case OP_IADD_IMM:
			if ((ins->inst_imm == 1) && (ins->dreg == ins->sreg1))
				ins->opcode = OP_X86_INC_REG;
			break;
		case OP_ISUB_IMM:
			if ((ins->inst_imm == 1) && (ins->dreg == ins->sreg1))
				ins->opcode = OP_X86_DEC_REG;
			break;
		}

		mono_peephole_ins (bb, ins);
	}
}

#define NEW_INS(cfg,ins,dest,op) do {	\
		MONO_INST_NEW ((cfg), (dest), (op)); \
		(dest)->cil_code = (ins)->cil_code; \
		mono_bblock_insert_before_ins (bb, ins, (dest)); \
	} while (0)

#define NEW_SIMD_INS(cfg,ins,dest,op,d,s1,s2) do {	\
		MONO_INST_NEW ((cfg), (dest), (op)); \
		(dest)->cil_code = (ins)->cil_code; \
		(dest)->dreg = d; \
		(dest)->sreg1 = s1; \
		(dest)->sreg2 = s2; \
		(dest)->type = STACK_VTYPE; \
		(dest)->klass = ins->klass; \
		mono_bblock_insert_before_ins (bb, ins, (dest)); \
	} while (0)

static int
simd_type_to_comp_op (int t)
{
	switch (t) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		return OP_PCMPEQB;
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		return OP_PCMPEQW;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return OP_PCMPEQD;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return OP_PCMPEQQ; // SSE 4.1
	default:
		g_assert_not_reached ();
		return -1;
	}
}

static int
simd_type_to_sub_op (int t)
{
	switch (t) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		return OP_PSUBB;
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		return OP_PSUBW;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return OP_PSUBD;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return OP_PSUBQ;
	default:
		g_assert_not_reached ();
		return -1;
	}
}

static int
simd_type_to_shl_op (int t)
{
	switch (t) {
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		return OP_PSHLW;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return OP_PSHLD;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return OP_PSHLQ;
	default:
		g_assert_not_reached ();
		return -1;
	}
}

static int
simd_type_to_gt_op (int t)
{
	switch (t) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		return OP_PCMPGTB;
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		return OP_PCMPGTW;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return OP_PCMPGTD;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return OP_PCMPGTQ; // SSE 4.2
	default:
		g_assert_not_reached ();
		return -1;
	}
}

static int
simd_type_to_max_un_op (int t)
{
	switch (t) {
	case MONO_TYPE_U1:
		return OP_PMAXB_UN;
	case MONO_TYPE_U2:
		return OP_PMAXW_UN; // SSE 4.1
	case MONO_TYPE_U4:
		return OP_PMAXD_UN; // SSE 4.1
	//case MONO_TYPE_U8:
	//	return OP_PMAXQ_UN; // AVX
	default:
		g_assert_not_reached ();
		return -1;
	}
}

static int
simd_type_to_add_op (int t)
{
	switch (t) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		return OP_PADDB;
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		return OP_PADDW;
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return OP_PADDD;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return OP_PADDQ;
	default:
		g_assert_not_reached ();
		return -1;
	}
}

static int
simd_type_to_min_op (int t)
{
	switch (t) {
	case MONO_TYPE_I1:
		return OP_PMINB; // SSE 4.1
	case MONO_TYPE_U1:
		return OP_PMINB_UN; // SSE 4.1
	case MONO_TYPE_I2:
		return OP_PMINW;
	case MONO_TYPE_U2:
		return OP_PMINW_UN;
	case MONO_TYPE_I4:
		return OP_PMIND; // SSE 4.1
	case MONO_TYPE_U4:
		return OP_PMIND_UN; // SSE 4.1
	// case MONO_TYPE_I8: // AVX
	// case MONO_TYPE_U8:
	default:
		g_assert_not_reached ();
		return -1;
	}
}

static int
simd_type_to_max_op (int t)
{
	switch (t) {
	case MONO_TYPE_I1:
		return OP_PMAXB; // SSE 4.1
	case MONO_TYPE_U1:
		return OP_PMAXB_UN; // SSE 4.1
	case MONO_TYPE_I2:
		return OP_PMAXW;
	case MONO_TYPE_U2:
		return OP_PMAXW_UN;
	case MONO_TYPE_I4:
		return OP_PMAXD; // SSE 4.1
	case MONO_TYPE_U4:
		return OP_PMAXD_UN; // SSE 4.1
	// case MONO_TYPE_I8: // AVX
	// case MONO_TYPE_U8:
	default:
		g_assert_not_reached ();
		return -1;
	}
}

static void
emit_simd_comp_op (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins, int type, int dreg, int sreg1, int sreg2)
{
	MonoInst *temp;

	if (!mono_hwcap_x86_has_sse42 && (ins->inst_c1 == MONO_TYPE_I8 || ins->inst_c1 == MONO_TYPE_U8)) {
		int temp_reg1 = mono_alloc_ireg (cfg);
		int temp_reg2 = mono_alloc_ireg (cfg);

		NEW_SIMD_INS (cfg, ins, temp, OP_PCMPEQD, temp_reg1, sreg1, sreg2);
		NEW_SIMD_INS (cfg, ins, temp, OP_PSHUFLED, temp_reg2, temp_reg1, -1);
		temp->inst_c0 = 0xB1;
		NEW_SIMD_INS (cfg, ins, temp, OP_ANDPD, dreg, temp_reg1, temp_reg2);
	} else {
		NEW_SIMD_INS (cfg, ins, temp, simd_type_to_comp_op (type), dreg, sreg1, sreg2);
	}
}

static void
emit_simd_gt_op (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins, int type, int dreg, int sreg1, int sreg2);

static void
emit_simd_gt_un_op (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins, int type, int dreg, int sreg1, int sreg2)
{
	MonoInst *temp;

	switch (type) {
		case MONO_TYPE_U2:
		case MONO_TYPE_U4:
			if (mono_hwcap_x86_has_sse41)
				goto USE_MAX;
			goto USE_SIGNED_GT;

		case MONO_TYPE_U1:
		USE_MAX: {
			// dreg = max(sreg1, sreg2) != sreg2
			
			int temp_reg1 = mono_alloc_ireg (cfg);
			int temp_reg2 = mono_alloc_ireg (cfg);
			int temp_reg3 = mono_alloc_ireg (cfg);

			NEW_SIMD_INS (cfg, ins, temp, simd_type_to_max_un_op (type), temp_reg1, sreg1, sreg2);
			emit_simd_comp_op (cfg, bb, ins, ins->inst_c1, temp_reg2, temp_reg1, ins->sreg2);
			NEW_SIMD_INS (cfg, ins, temp, OP_XONES, temp_reg3, -1, -1);
			NEW_SIMD_INS (cfg, ins, temp, OP_XORPD, dreg, temp_reg2, temp_reg3);
			break;
		}

		case MONO_TYPE_U8:
		USE_SIGNED_GT: {
			// convert to signed integer by subtracting (1 << (size - 1)) from each operand
			// and then use signed comparison

			int temp_c0 = mono_alloc_ireg (cfg);
			int temp_c80 = mono_alloc_ireg (cfg);
			int temp_s1 = mono_alloc_ireg (cfg);
			int temp_s2 = mono_alloc_ireg (cfg);

			NEW_SIMD_INS (cfg, ins, temp, OP_XONES, temp_c0, -1, -1);
			NEW_SIMD_INS (cfg, ins, temp, simd_type_to_shl_op (type), temp_c80, temp_c0, -1);
			temp->inst_imm = type == MONO_TYPE_U2 ? 15 : (type == MONO_TYPE_U4 ? 31 : 63);
			NEW_SIMD_INS (cfg, ins, temp, simd_type_to_sub_op (type), temp_s1, sreg1, temp_c80);
			NEW_SIMD_INS (cfg, ins, temp, simd_type_to_sub_op (type), temp_s2, sreg2, temp_c80);
			emit_simd_gt_op (cfg, bb, ins, type, dreg, temp_s1, temp_s2);
			break;
		}
	}
}

static void
emit_simd_gt_op (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins, int type, int dreg, int sreg1, int sreg2)
{
	MonoInst *temp;

	if (!mono_hwcap_x86_has_sse42 && (type == MONO_TYPE_I8 || type == MONO_TYPE_U8)) {
		// Decompose 64-bit greater than to 32-bit
		//
		// t = (v1 > v2)
		// u = (v1 == v2)
		// v = (v1 > v2) unsigned
		//
		// z = shuffle(t, (3, 3, 1, 1))
		// t1 = shuffle(v, (2, 2, 0, 0))
		// u1 = shuffle(u, (3, 3, 1, 1))
		// w = and(t1, u1)
		// result = bitwise_or(z, w)

		int temp_t = mono_alloc_ireg (cfg);
		int temp_u = mono_alloc_ireg (cfg);
		int temp_v = mono_alloc_ireg (cfg);
		int temp_z = temp_t;
		int temp_t1 = temp_v;
		int temp_u1 = temp_u;
		int temp_w = temp_t1;

		NEW_SIMD_INS (cfg, ins, temp, OP_PCMPGTD, temp_t, sreg1, sreg2);
		NEW_SIMD_INS (cfg, ins, temp, OP_PCMPEQD, temp_u, sreg1, sreg2);
		emit_simd_gt_un_op (cfg, bb, ins, MONO_TYPE_U4, temp_v, sreg1, sreg2);
		NEW_SIMD_INS (cfg, ins, temp, OP_PSHUFLED, temp_z, temp_t, -1);
		temp->inst_c0 = 0xF5;
		NEW_SIMD_INS (cfg, ins, temp, OP_PSHUFLED, temp_t1, temp_v, -1);
		temp->inst_c0 = 0xA0;
		NEW_SIMD_INS (cfg, ins, temp, OP_PSHUFLED, temp_u1, temp_u, -1);
		temp->inst_c0 = 0xF5;
		NEW_SIMD_INS (cfg, ins, temp, OP_ANDPD, temp_w, temp_t1, temp_u1);
		NEW_SIMD_INS (cfg, ins, temp, OP_ORPD, dreg, temp_z, temp_w);
	} else {
		NEW_SIMD_INS (cfg, ins, temp, simd_type_to_gt_op (type), dreg, sreg1, sreg2);
	}
}

static void
emit_simd_min_op (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins, int type, int dreg, int sreg1, int sreg2)
{
	MonoInst *temp;

	if (type == MONO_TYPE_I2 || type == MONO_TYPE_U2) {
		// SSE2, so always available
		NEW_SIMD_INS (cfg, ins, temp, simd_type_to_min_op (type), dreg, sreg1, sreg2);
	} else if (!mono_hwcap_x86_has_sse41 || type == MONO_TYPE_I8 || type == MONO_TYPE_U8) {
		// Decompose to t = (s1 > s2), d = (s1 & !t) | (s2 & t)
		int temp_t = mono_alloc_ireg (cfg);
		int temp_d1 = mono_alloc_ireg (cfg);
		int temp_d2 = mono_alloc_ireg (cfg);
		if (type == MONO_TYPE_U8 || type == MONO_TYPE_U4 || type == MONO_TYPE_U1)
			emit_simd_gt_un_op (cfg, bb, ins, type, temp_t, sreg1, sreg2);
		else
			emit_simd_gt_op (cfg, bb, ins, type, temp_t, sreg1, sreg2);
		NEW_SIMD_INS (cfg, ins, temp, OP_PANDN, temp_d1, temp_t, sreg1);
		NEW_SIMD_INS (cfg, ins, temp, OP_PAND, temp_d2, temp_t, sreg2);
		NEW_SIMD_INS (cfg, ins, temp, OP_POR, dreg, temp_d1, temp_d2);
	} else {
		// SSE 4.1 has byte- and dword- operations
		NEW_SIMD_INS (cfg, ins, temp, simd_type_to_min_op (type), dreg, sreg1, sreg2);
	}
}

static void
emit_simd_max_op (MonoCompile *cfg, MonoBasicBlock *bb, MonoInst *ins, int type, int dreg, int sreg1, int sreg2)
{
	MonoInst *temp;

	if (type == MONO_TYPE_I2 || type == MONO_TYPE_U2) {
		// SSE2, so always available
		NEW_SIMD_INS (cfg, ins, temp, simd_type_to_max_op (type), dreg, sreg1, sreg2);
	} else if (!mono_hwcap_x86_has_sse41 || type == MONO_TYPE_I8 || type == MONO_TYPE_U8) {
		// Decompose to t = (s1 > s2), d = (s1 & t) | (s2 & !t)
		int temp_t = mono_alloc_ireg (cfg);
		int temp_d1 = mono_alloc_ireg (cfg);
		int temp_d2 = mono_alloc_ireg (cfg);
		if (type == MONO_TYPE_U8 || type == MONO_TYPE_U4 || type == MONO_TYPE_U1)
			emit_simd_gt_un_op (cfg, bb, ins, type, temp_t, sreg1, sreg2);
		else
			emit_simd_gt_op (cfg, bb, ins, type, temp_t, sreg1, sreg2);
		NEW_SIMD_INS (cfg, ins, temp, OP_PAND, temp_d1, temp_t, sreg1);
		NEW_SIMD_INS (cfg, ins, temp, OP_PANDN, temp_d2, temp_t, sreg2);
		NEW_SIMD_INS (cfg, ins, temp, OP_POR, dreg, temp_d1, temp_d2);
	} else {
		// SSE 4.1 has byte- and dword- operations
		NEW_SIMD_INS (cfg, ins, temp, simd_type_to_max_op (type), dreg, sreg1, sreg2);
	}
}

/*
 * mono_arch_lowering_pass:
 *
 *  Converts complex opcodes into simpler ones so that each IR instruction
 * corresponds to one machine instruction.
 */
void
mono_arch_lowering_pass (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins, *n, *temp;

	/*
	 * FIXME: Need to add more instructions, but the current machine 
	 * description can't model some parts of the composite instructions like
	 * cdq.
	 */
	MONO_BB_FOR_EACH_INS_SAFE (bb, n, ins) {
		switch (ins->opcode) {
		case OP_DIV_IMM:
		case OP_REM_IMM:
		case OP_IDIV_IMM:
		case OP_IDIV_UN_IMM:
		case OP_IREM_UN_IMM:
		case OP_LREM_IMM:
		case OP_IREM_IMM:
			mono_decompose_op_imm (cfg, bb, ins);
			break;
		case OP_COMPARE_IMM:
		case OP_LCOMPARE_IMM:
			if (!amd64_use_imm32 (ins->inst_imm)) {
				NEW_INS (cfg, ins, temp, OP_I8CONST);
				temp->inst_c0 = ins->inst_imm;
				temp->dreg = mono_alloc_ireg (cfg);
				ins->opcode = OP_COMPARE;
				ins->sreg2 = temp->dreg;
			}
			break;
#ifndef MONO_ARCH_ILP32
		case OP_LOAD_MEMBASE:
#endif
		case OP_LOADI8_MEMBASE:
		/*  Don't generate memindex opcodes (to simplify */
		/*  read sandboxing) */
			if (!amd64_use_imm32 (ins->inst_offset)) {
				NEW_INS (cfg, ins, temp, OP_I8CONST);
				temp->inst_c0 = ins->inst_offset;
				temp->dreg = mono_alloc_ireg (cfg);
				ins->opcode = OP_AMD64_LOADI8_MEMINDEX;
				ins->inst_indexreg = temp->dreg;
			}
			break;
#ifndef MONO_ARCH_ILP32
		case OP_STORE_MEMBASE_IMM:
#endif
		case OP_STOREI8_MEMBASE_IMM:
			if (!amd64_use_imm32 (ins->inst_imm)) {
				NEW_INS (cfg, ins, temp, OP_I8CONST);
				temp->inst_c0 = ins->inst_imm;
				temp->dreg = mono_alloc_ireg (cfg);
				ins->opcode = OP_STOREI8_MEMBASE_REG;
				ins->sreg1 = temp->dreg;
			}
			break;
#ifdef MONO_ARCH_SIMD_INTRINSICS
		case OP_EXPAND_I1: {
			int temp_reg1 = mono_alloc_ireg (cfg);
			int temp_reg2 = mono_alloc_ireg (cfg);
			int original_reg = ins->sreg1;

			NEW_INS (cfg, ins, temp, OP_ICONV_TO_U1);
			temp->sreg1 = original_reg;
			temp->dreg = temp_reg1;

			NEW_INS (cfg, ins, temp, OP_SHL_IMM);
			temp->sreg1 = temp_reg1;
			temp->dreg = temp_reg2;
			temp->inst_imm = 8;

			NEW_INS (cfg, ins, temp, OP_LOR);
			temp->sreg1 = temp->dreg = temp_reg2;
			temp->sreg2 = temp_reg1;

 			ins->opcode = OP_EXPAND_I2;
			ins->sreg1 = temp_reg2;
			break;
		}

		case OP_XEQUAL: {
			int temp_reg1 = mono_alloc_ireg (cfg);
			int temp_reg2 = mono_alloc_ireg (cfg);

			NEW_SIMD_INS (cfg, ins, temp, OP_PCMPEQD, temp_reg1, ins->sreg1, ins->sreg2);
			NEW_SIMD_INS (cfg, ins, temp, OP_EXTRACT_MASK, temp_reg2, temp_reg1, -1);
			temp->type = STACK_I4;
			NEW_INS (cfg, ins, temp, OP_COMPARE_IMM);
			temp->sreg1 = temp_reg2;
			temp->inst_imm = 0xFFFF;
			temp->klass = ins->klass;
			ins->opcode = OP_CEQ;
			ins->sreg1 = -1;
			ins->sreg2 = -1;
			break;
		}

		case OP_XCOMPARE: {
			int temp_reg;

			switch (ins->inst_c0)
			{
			case CMP_EQ:
				emit_simd_comp_op (cfg, bb, ins, ins->inst_c1, ins->dreg, ins->sreg1, ins->sreg2);
				NULLIFY_INS (ins);
				break;

			case CMP_NE: {
				int temp_reg1 = mono_alloc_ireg (cfg);
				int temp_reg2 = mono_alloc_ireg (cfg);

				emit_simd_comp_op (cfg, bb, ins, ins->inst_c1, temp_reg1, ins->sreg1, ins->sreg2);
				NEW_SIMD_INS (cfg, ins, temp, OP_XONES, temp_reg2, -1, -1);
				ins->opcode = OP_XORPD;
				ins->sreg1 = temp_reg1;
				ins->sreg1 = temp_reg2;
				break;
			}

			case CMP_LT:
				temp_reg = ins->sreg1;
				ins->sreg1 = ins->sreg2;
				ins->sreg2 = temp_reg;
			case CMP_GT:
				emit_simd_gt_op (cfg, bb, ins, ins->inst_c1, ins->dreg, ins->sreg1, ins->sreg2);
				NULLIFY_INS (ins);
				break;

			case CMP_LE:
				temp_reg = ins->sreg1;
				ins->sreg1 = ins->sreg2;
				ins->sreg2 = temp_reg;
			case CMP_GE: {
				int temp_reg1 = mono_alloc_ireg (cfg);
				int temp_reg2 = mono_alloc_ireg (cfg);

				emit_simd_gt_op (cfg, bb, ins, ins->inst_c1, temp_reg1, ins->sreg1, ins->sreg2);
				emit_simd_comp_op (cfg, bb, ins, ins->inst_c1, temp_reg2, ins->sreg1, ins->sreg2);
				ins->opcode = OP_POR;
				ins->sreg1 = temp_reg1;
				ins->sreg2 = temp_reg2;
				break;
			}

			case CMP_LE_UN:
				temp_reg = ins->sreg1;
				ins->sreg1 = ins->sreg2;
				ins->sreg2 = temp_reg;
			case CMP_GE_UN:
				if (mono_hwcap_x86_has_sse41 && ins->inst_c1 != MONO_TYPE_U8) {
					int temp_reg1 = mono_alloc_ireg (cfg);

					NEW_SIMD_INS (cfg, ins, temp, simd_type_to_max_un_op (ins->inst_c1), temp_reg1, ins->sreg1, ins->sreg2);
					emit_simd_comp_op (cfg, bb, ins, ins->inst_c1, ins->dreg, temp_reg1, ins->sreg1);
					NULLIFY_INS (ins);
				} else {
					int temp_reg1 = mono_alloc_ireg (cfg);
					int temp_reg2 = mono_alloc_ireg (cfg);

					emit_simd_gt_un_op (cfg, bb, ins, ins->inst_c1, temp_reg1, ins->sreg1, ins->sreg2);
					emit_simd_comp_op (cfg, bb, ins, ins->inst_c1, temp_reg2, ins->sreg1, ins->sreg2);
					ins->opcode = OP_POR;
					ins->sreg1 = temp_reg1;
					ins->sreg2 = temp_reg2;
				}
				break;

			case CMP_LT_UN:
				temp_reg = ins->sreg1;
				ins->sreg1 = ins->sreg2;
				ins->sreg2 = temp_reg;
			case CMP_GT_UN: {
				emit_simd_gt_un_op (cfg, bb, ins, ins->inst_c1, ins->dreg, ins->sreg1, ins->sreg2);
				NULLIFY_INS (ins);
				break;
			}

			default:
				g_assert_not_reached();
				break;
			}

			ins->type = STACK_VTYPE;
			ins->inst_c0 = 0;
			break;
		}

		case OP_XCOMPARE_FP: {
			ins->opcode = ins->inst_c1 == MONO_TYPE_R4 ? OP_COMPPS : OP_COMPPD;

			switch (ins->inst_c0)
			{
			case CMP_EQ: ins->inst_c0 = 0; break;
			case CMP_NE: ins->inst_c0 = 4; break;
			case CMP_LT: ins->inst_c0 = 1; break;
			case CMP_LE: ins->inst_c0 = 2; break;
			case CMP_GT: ins->inst_c0 = 6; break;
			case CMP_GE: ins->inst_c0 = 5; break;
			default:
				g_assert_not_reached();
				break;
			}

			break;
		}

		case OP_XCAST: {
			ins->opcode = OP_XMOVE;
			break;
		}

		case OP_XBINOP: {
			switch (ins->inst_c0)
			{
			case OP_ISUB:
				ins->opcode = simd_type_to_sub_op (ins->inst_c1);
				break;
			case OP_IADD:
				ins->opcode = simd_type_to_add_op (ins->inst_c1);
				break;
			case OP_IAND:
				ins->opcode = OP_ANDPD;
				break;
			case OP_IXOR:
				ins->opcode = OP_XORPD;
				break;
			case OP_IOR:
				ins->opcode = OP_ORPD;
				break;
			case OP_IMIN:
				emit_simd_min_op (cfg, bb, ins, ins->inst_c1, ins->dreg, ins->sreg1, ins->sreg2);
				NULLIFY_INS (ins);
				break;
			case OP_IMAX:
				emit_simd_max_op (cfg, bb, ins, ins->inst_c1, ins->dreg, ins->sreg1, ins->sreg2);
				NULLIFY_INS (ins);
				break;
			case OP_FSUB:
				ins->opcode = ins->inst_c1 == MONO_TYPE_R8 ? OP_SUBPD : OP_SUBPS;
				break;
			case OP_FADD:
				ins->opcode = ins->inst_c1 == MONO_TYPE_R8 ? OP_ADDPD : OP_ADDPS;
				break;
			case OP_FDIV:
				ins->opcode = ins->inst_c1 == MONO_TYPE_R8 ? OP_DIVPD : OP_DIVPS;
				break;
			case OP_FMUL:
				ins->opcode = ins->inst_c1 == MONO_TYPE_R8 ? OP_MULPD : OP_MULPS;
				break;
			case OP_FMIN:
				ins->opcode = ins->inst_c1 == MONO_TYPE_R8 ? OP_MINPD : OP_MINPS;
				break;
			case OP_FMAX:
				ins->opcode = ins->inst_c1 == MONO_TYPE_R8 ? OP_MAXPD : OP_MAXPS;
				break;
			default:
				g_assert_not_reached();
				break;
			}
			break;
		}

		case OP_XEXTRACT_R4:
		case OP_XEXTRACT_R8:
		case OP_XEXTRACT_I32:
		case OP_XEXTRACT_I64: {
			// TODO
			g_assert_not_reached();
			break;
		}
#endif
		default:
			break;
		}
	}

	bb->max_vreg = cfg->next_vreg;
}

static const int 
branch_cc_table [] = {
	X86_CC_EQ, X86_CC_GE, X86_CC_GT, X86_CC_LE, X86_CC_LT,
	X86_CC_NE, X86_CC_GE, X86_CC_GT, X86_CC_LE, X86_CC_LT,
	X86_CC_O, X86_CC_NO, X86_CC_C, X86_CC_NC
};

/* Maps CMP_... constants to X86_CC_... constants */
static const int
cc_table [] = {
	X86_CC_EQ, X86_CC_NE, X86_CC_LE, X86_CC_GE, X86_CC_LT, X86_CC_GT,
	X86_CC_LE, X86_CC_GE, X86_CC_LT, X86_CC_GT
};

static const int
cc_signed_table [] = {
	TRUE, TRUE, TRUE, TRUE, TRUE, TRUE,
	FALSE, FALSE, FALSE, FALSE
};

/*#include "cprop.c"*/

static unsigned char*
emit_float_to_int (MonoCompile *cfg, guchar *code, int dreg, int sreg, int size, gboolean is_signed)
{
	// Use 8 as register size to get Nan/Inf conversion to uint result truncated to 0
	if (size == 8 || (!is_signed && size == 4))
		amd64_sse_cvttsd2si_reg_reg (code, dreg, sreg);
	else
		amd64_sse_cvttsd2si_reg_reg_size (code, dreg, sreg, 4);

	if (size == 1)
		amd64_widen_reg (code, dreg, dreg, is_signed, FALSE);
	else if (size == 2)
		amd64_widen_reg (code, dreg, dreg, is_signed, TRUE);
	return code;
}

static unsigned char*
mono_emit_stack_alloc (MonoCompile *cfg, guchar *code, MonoInst* tree)
{
	int sreg = tree->sreg1;
	int need_touch = FALSE;

#if defined(TARGET_WIN32)
	need_touch = TRUE;
#elif defined(MONO_ARCH_SIGSEGV_ON_ALTSTACK)
	if (!(tree->flags & MONO_INST_INIT))
		need_touch = TRUE;
#endif

	if (need_touch) {
		guint8* br[5];

		/*
		 * Under Windows:
		 * If requested stack size is larger than one page,
		 * perform stack-touch operation
		 */
		/*
		 * Generate stack probe code.
		 * Under Windows, it is necessary to allocate one page at a time,
		 * "touching" stack after each successful sub-allocation. This is
		 * because of the way stack growth is implemented - there is a
		 * guard page before the lowest stack page that is currently commited.
		 * Stack normally grows sequentially so OS traps access to the
		 * guard page and commits more pages when needed.
		 */
		amd64_test_reg_imm (code, sreg, ~0xFFF);
		br[0] = code; x86_branch8 (code, X86_CC_Z, 0, FALSE);

		br[2] = code; /* loop */
		amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, 0x1000);
		amd64_test_membase_reg (code, AMD64_RSP, 0, AMD64_RSP);
		amd64_alu_reg_imm (code, X86_SUB, sreg, 0x1000);
		amd64_alu_reg_imm (code, X86_CMP, sreg, 0x1000);
		br[3] = code; x86_branch8 (code, X86_CC_AE, 0, FALSE);
		amd64_patch (br[3], br[2]);
		amd64_test_reg_reg (code, sreg, sreg);
		br[4] = code; x86_branch8 (code, X86_CC_Z, 0, FALSE);
		amd64_alu_reg_reg (code, X86_SUB, AMD64_RSP, sreg);

		br[1] = code; x86_jump8 (code, 0);

		amd64_patch (br[0], code);
		amd64_alu_reg_reg (code, X86_SUB, AMD64_RSP, sreg);
		amd64_patch (br[1], code);
		amd64_patch (br[4], code);
	}
	else
		amd64_alu_reg_reg (code, X86_SUB, AMD64_RSP, tree->sreg1);

	if (tree->flags & MONO_INST_INIT) {
		int offset = 0;
		if (tree->dreg != AMD64_RAX && sreg != AMD64_RAX) {
			amd64_push_reg (code, AMD64_RAX);
			offset += 8;
		}
		if (tree->dreg != AMD64_RCX && sreg != AMD64_RCX) {
			amd64_push_reg (code, AMD64_RCX);
			offset += 8;
		}
		if (tree->dreg != AMD64_RDI && sreg != AMD64_RDI) {
			amd64_push_reg (code, AMD64_RDI);
			offset += 8;
		}
		
		amd64_shift_reg_imm (code, X86_SHR, sreg, 3);
		if (sreg != AMD64_RCX)
			amd64_mov_reg_reg (code, AMD64_RCX, sreg, 8);
		amd64_alu_reg_reg (code, X86_XOR, AMD64_RAX, AMD64_RAX);
				
		amd64_lea_membase (code, AMD64_RDI, AMD64_RSP, offset);
		if (cfg->param_area)
			amd64_alu_reg_imm (code, X86_ADD, AMD64_RDI, cfg->param_area);
		amd64_cld (code);
		amd64_prefix (code, X86_REP_PREFIX);
		amd64_stosl (code);
		
		if (tree->dreg != AMD64_RDI && sreg != AMD64_RDI)
			amd64_pop_reg (code, AMD64_RDI);
		if (tree->dreg != AMD64_RCX && sreg != AMD64_RCX)
			amd64_pop_reg (code, AMD64_RCX);
		if (tree->dreg != AMD64_RAX && sreg != AMD64_RAX)
			amd64_pop_reg (code, AMD64_RAX);
	}
	return code;
}

static guint8*
emit_move_return_value (MonoCompile *cfg, MonoInst *ins, guint8 *code)
{
	CallInfo *cinfo;
	guint32 quad;

	/* Move return value to the target register */
	/* FIXME: do this in the local reg allocator */
	switch (ins->opcode) {
	case OP_CALL:
	case OP_CALL_REG:
	case OP_CALL_MEMBASE:
	case OP_LCALL:
	case OP_LCALL_REG:
	case OP_LCALL_MEMBASE:
		g_assert (ins->dreg == AMD64_RAX);
		break;
	case OP_FCALL:
	case OP_FCALL_REG:
	case OP_FCALL_MEMBASE: {
		MonoType *rtype = mini_get_underlying_type (((MonoCallInst*)ins)->signature->ret);
		if (rtype->type == MONO_TYPE_R4) {
			amd64_sse_cvtss2sd_reg_reg (code, ins->dreg, AMD64_XMM0);
		}
		else {
			if (ins->dreg != AMD64_XMM0)
				amd64_sse_movsd_reg_reg (code, ins->dreg, AMD64_XMM0);
		}
		break;
	}
	case OP_RCALL:
	case OP_RCALL_REG:
	case OP_RCALL_MEMBASE:
		if (ins->dreg != AMD64_XMM0)
			amd64_sse_movss_reg_reg (code, ins->dreg, AMD64_XMM0);
		break;
	case OP_VCALL:
	case OP_VCALL_REG:
	case OP_VCALL_MEMBASE:
	case OP_VCALL2:
	case OP_VCALL2_REG:
	case OP_VCALL2_MEMBASE:
		cinfo = mono_arch_get_call_info (cfg->mempool, ((MonoCallInst*)ins)->signature);
		if (cinfo->ret.storage == ArgValuetypeInReg) {
			MonoInst *loc = cfg->arch.vret_addr_loc;

			/* Load the destination address */
			g_assert (loc->opcode == OP_REGOFFSET);
			amd64_mov_reg_membase (code, AMD64_RCX, loc->inst_basereg, loc->inst_offset, sizeof(gpointer));

			for (quad = 0; quad < 2; quad ++) {
				switch (cinfo->ret.pair_storage [quad]) {
				case ArgInIReg:
					amd64_mov_membase_reg (code, AMD64_RCX, (quad * sizeof (target_mgreg_t)), cinfo->ret.pair_regs [quad], sizeof (target_mgreg_t));
					break;
				case ArgInFloatSSEReg:
					amd64_movss_membase_reg (code, AMD64_RCX, (quad * 8), cinfo->ret.pair_regs [quad]);
					break;
				case ArgInDoubleSSEReg:
					amd64_movsd_membase_reg (code, AMD64_RCX, (quad * 8), cinfo->ret.pair_regs [quad]);
					break;
				case ArgNone:
					break;
				default:
					NOT_IMPLEMENTED;
				}
			}
		}
		break;
	}

	return code;
}

#endif /* DISABLE_JIT */

/*
 * emit_setup_lmf:
 *
 *   Emit code to initialize an LMF structure at LMF_OFFSET.
 */
static guint8*
emit_setup_lmf (MonoCompile *cfg, guint8 *code, gint32 lmf_offset, int cfa_offset)
{
	/* 
	 * The ip field is not set, the exception handling code will obtain it from the stack location pointed to by the sp field.
	 */
	/* 
	 * sp is saved right before calls but we need to save it here too so
	 * async stack walks would work.
	 */
	amd64_mov_membase_reg (code, cfg->frame_reg, lmf_offset + MONO_STRUCT_OFFSET (MonoLMF, rsp), AMD64_RSP, 8);
	/* Save rbp */
	amd64_mov_membase_reg (code, cfg->frame_reg, lmf_offset + MONO_STRUCT_OFFSET (MonoLMF, rbp), AMD64_RBP, 8);
	if (cfg->arch.omit_fp && cfa_offset != -1)
		mono_emit_unwind_op_offset (cfg, code, AMD64_RBP, - (cfa_offset - (lmf_offset + MONO_STRUCT_OFFSET (MonoLMF, rbp))));

	/* These can't contain refs */
	mini_gc_set_slot_type_from_fp (cfg, lmf_offset + MONO_STRUCT_OFFSET (MonoLMF, previous_lmf), SLOT_NOREF);
	mini_gc_set_slot_type_from_fp (cfg, lmf_offset + MONO_STRUCT_OFFSET (MonoLMF, rsp), SLOT_NOREF);
	/* These are handled automatically by the stack marking code */
	mini_gc_set_slot_type_from_fp (cfg, lmf_offset + MONO_STRUCT_OFFSET (MonoLMF, rbp), SLOT_NOREF);

	return code;
}

#ifdef TARGET_WIN32

#define TEB_LAST_ERROR_OFFSET 0x68

static guint8*
emit_get_last_error (guint8* code, int dreg)
{
	/* Threads last error value is located in TEB_LAST_ERROR_OFFSET. */
	x86_prefix (code, X86_GS_PREFIX);
	amd64_mov_reg_mem (code, dreg, TEB_LAST_ERROR_OFFSET, sizeof (guint32));
	return code;
}

#else

static guint8*
emit_get_last_error (guint8* code, int dreg)
{
	g_assert_not_reached ();
}

#endif

/* benchmark and set based on cpu */
#define LOOP_ALIGNMENT 8
#define bb_is_loop_start(bb) ((bb)->loop_body_start && (bb)->nesting)

#ifndef DISABLE_JIT

static guint8*
amd64_handle_varargs_nregs (guint8 *code, guint32 nregs)
{
#ifndef TARGET_WIN32
	if (nregs)
		amd64_mov_reg_imm (code, AMD64_RAX, nregs);
	else
		amd64_alu_reg_reg (code, X86_XOR, AMD64_RAX, AMD64_RAX);
#endif
	return code;
}

static guint8*
amd64_handle_varargs_call (MonoCompile *cfg, guint8 *code, MonoCallInst *call, gboolean free_rax)
{
#ifdef TARGET_WIN32
	return code;
#else
	/*
	 * The AMD64 ABI forces callers to know about varargs.
	 */
	guint32 nregs = 0;
	if (call->signature->call_convention == MONO_CALL_VARARG && call->signature->pinvoke) {
		// deliberatly nothing -- but nreg = 0 and do not return
	} else if (cfg->method->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE && m_class_get_image (cfg->method->klass) != mono_defaults.corlib) {
		/*
		 * Since the unmanaged calling convention doesn't contain a
		 * 'vararg' entry, we have to treat every pinvoke call as a
		 * potential vararg call.
		 */
		for (guint32 i = 0; i < AMD64_XMM_NREG; ++i)
			nregs += (call->used_fregs & (1 << i)) != 0;
	} else {
		return code;
	}
	MonoInst *ins = (MonoInst*)call;
	if (free_rax && ins->sreg1 == AMD64_RAX) {
		amd64_mov_reg_reg (code, AMD64_R11, AMD64_RAX, 8);
		ins->sreg1 = AMD64_R11;
	}
	return amd64_handle_varargs_nregs (code, nregs);
#endif
}

void
mono_arch_output_basic_block (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins;
	MonoCallInst *call;
	guint8 *code = cfg->native_code + cfg->code_len;

	/* Fix max_offset estimate for each successor bb */
	gboolean optimize_branch_pred = (cfg->opt & MONO_OPT_BRANCH) && (cfg->max_block_num < MAX_BBLOCKS_FOR_BRANCH_OPTS);

	if (optimize_branch_pred) {
		int current_offset = cfg->code_len;
		MonoBasicBlock *current_bb;
		for (current_bb = bb; current_bb != NULL; current_bb = current_bb->next_bb) {
			current_bb->max_offset = current_offset;
			current_offset += current_bb->max_length;
		}
	}

	if (cfg->opt & MONO_OPT_LOOP) {
		int pad, align = LOOP_ALIGNMENT;
		/* set alignment depending on cpu */
		if (bb_is_loop_start (bb) && (pad = (cfg->code_len & (align - 1)))) {
			pad = align - pad;
			/*g_print ("adding %d pad at %x to loop in %s\n", pad, cfg->code_len, cfg->method->name);*/
			amd64_padding (code, pad);
			cfg->code_len += pad;
			bb->native_offset = cfg->code_len;
		}
	}

	if (cfg->verbose_level > 2)
		g_print ("Basic block %d starting at offset 0x%x\n", bb->block_num, bb->native_offset);

	set_code_cursor (cfg, code);

	mono_debug_open_block (cfg, bb, code - cfg->native_code);

    if (mono_break_at_bb_method && mono_method_desc_full_match (mono_break_at_bb_method, cfg->method) && bb->block_num == mono_break_at_bb_bb_num)
		x86_breakpoint (code);

	MONO_BB_FOR_EACH_INS (bb, ins) {
		const guint offset = code - cfg->native_code;
		set_code_cursor (cfg, code);
		int max_len = ins_get_size (ins->opcode);

		code = realloc_code (cfg, max_len);

		if (cfg->debug_info)
			mono_debug_record_line_number (cfg, ins, offset);

		switch (ins->opcode) {
		case OP_BIGMUL:
			amd64_mul_reg (code, ins->sreg2, TRUE);
			break;
		case OP_BIGMUL_UN:
			amd64_mul_reg (code, ins->sreg2, FALSE);
			break;
		case OP_X86_SETEQ_MEMBASE:
			amd64_set_membase (code, X86_CC_EQ, ins->inst_basereg, ins->inst_offset, TRUE);
			break;
		case OP_STOREI1_MEMBASE_IMM:
			amd64_mov_membase_imm (code, ins->inst_destbasereg, ins->inst_offset, ins->inst_imm, 1);
			break;
		case OP_STOREI2_MEMBASE_IMM:
			amd64_mov_membase_imm (code, ins->inst_destbasereg, ins->inst_offset, ins->inst_imm, 2);
			break;
		case OP_STOREI4_MEMBASE_IMM:
			amd64_mov_membase_imm (code, ins->inst_destbasereg, ins->inst_offset, ins->inst_imm, 4);
			break;
		case OP_STOREI1_MEMBASE_REG:
			amd64_mov_membase_reg (code, ins->inst_destbasereg, ins->inst_offset, ins->sreg1, 1);
			break;
		case OP_STOREI2_MEMBASE_REG:
			amd64_mov_membase_reg (code, ins->inst_destbasereg, ins->inst_offset, ins->sreg1, 2);
			break;
		/* In AMD64 NaCl, pointers are 4 bytes, */
		/*  so STORE_* != STOREI8_*. Likewise below. */
		case OP_STORE_MEMBASE_REG:
			amd64_mov_membase_reg (code, ins->inst_destbasereg, ins->inst_offset, ins->sreg1, sizeof(gpointer));
			break;
		case OP_STOREI8_MEMBASE_REG:
			amd64_mov_membase_reg (code, ins->inst_destbasereg, ins->inst_offset, ins->sreg1, 8);
			break;
		case OP_STOREI4_MEMBASE_REG:
			amd64_mov_membase_reg (code, ins->inst_destbasereg, ins->inst_offset, ins->sreg1, 4);
			break;
		case OP_STORE_MEMBASE_IMM:
			/* In NaCl, this could be a PCONST type, which could */
			/* mean a pointer type was copied directly into the  */
			/* lower 32-bits of inst_imm, so for InvalidPtr==-1  */
			/* the value would be 0x00000000FFFFFFFF which is    */
			/* not proper for an imm32 unless you cast it.       */
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_mov_membase_imm (code, ins->inst_destbasereg, ins->inst_offset, (gint32)ins->inst_imm, sizeof(gpointer));
			break;
		case OP_STOREI8_MEMBASE_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_mov_membase_imm (code, ins->inst_destbasereg, ins->inst_offset, ins->inst_imm, 8);
			break;
		case OP_LOAD_MEM:
#ifdef MONO_ARCH_ILP32
			/* In ILP32, pointers are 4 bytes, so separate these */
			/* cases, use literal 8 below where we really want 8 */
			amd64_mov_reg_imm (code, ins->dreg, ins->inst_imm);
			amd64_mov_reg_membase (code, ins->dreg, ins->dreg, 0, sizeof(gpointer));
			break;
#endif
		case OP_LOADI8_MEM:
			// FIXME: Decompose this earlier
			if (amd64_use_imm32 (ins->inst_imm))
				amd64_mov_reg_mem (code, ins->dreg, ins->inst_imm, 8);
			else {
				amd64_mov_reg_imm_size (code, ins->dreg, ins->inst_imm, sizeof(gpointer));
				amd64_mov_reg_membase (code, ins->dreg, ins->dreg, 0, 8);
			}
			break;
		case OP_LOADI4_MEM:
			amd64_mov_reg_imm (code, ins->dreg, ins->inst_imm);
			amd64_movsxd_reg_membase (code, ins->dreg, ins->dreg, 0);
			break;
		case OP_LOADU4_MEM:
			// FIXME: Decompose this earlier
			if (amd64_use_imm32 (ins->inst_imm))
				amd64_mov_reg_mem (code, ins->dreg, ins->inst_imm, 4);
			else {
				amd64_mov_reg_imm_size (code, ins->dreg, ins->inst_imm, sizeof(gpointer));
				amd64_mov_reg_membase (code, ins->dreg, ins->dreg, 0, 4);
			}
			break;
		case OP_LOADU1_MEM:
			amd64_mov_reg_imm (code, ins->dreg, ins->inst_imm);
			amd64_widen_membase (code, ins->dreg, ins->dreg, 0, FALSE, FALSE);
			break;
		case OP_LOADU2_MEM:
			/* For NaCl, pointers are 4 bytes, so separate these */
			/* cases, use literal 8 below where we really want 8 */
			amd64_mov_reg_imm (code, ins->dreg, ins->inst_imm);
			amd64_widen_membase (code, ins->dreg, ins->dreg, 0, FALSE, TRUE);
			break;
		case OP_LOAD_MEMBASE:
			g_assert (amd64_is_imm32 (ins->inst_offset));
			amd64_mov_reg_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset, sizeof(gpointer));
			break;
		case OP_LOADI8_MEMBASE:
			/* Use literal 8 instead of sizeof pointer or */
			/* register, we really want 8 for this opcode */
			g_assert (amd64_is_imm32 (ins->inst_offset));
			amd64_mov_reg_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset, 8);
			break;
		case OP_LOADI4_MEMBASE:
			amd64_movsxd_reg_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			break;
		case OP_LOADU4_MEMBASE:
			amd64_mov_reg_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset, 4);
			break;
		case OP_LOADU1_MEMBASE:
			/* The cpu zero extends the result into 64 bits */
			amd64_widen_membase_size (code, ins->dreg, ins->inst_basereg, ins->inst_offset, FALSE, FALSE, 4);
			break;
		case OP_LOADI1_MEMBASE:
			amd64_widen_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset, TRUE, FALSE);
			break;
		case OP_LOADU2_MEMBASE:
			/* The cpu zero extends the result into 64 bits */
			amd64_widen_membase_size (code, ins->dreg, ins->inst_basereg, ins->inst_offset, FALSE, TRUE, 4);
			break;
		case OP_LOADI2_MEMBASE:
			amd64_widen_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset, TRUE, TRUE);
			break;
		case OP_AMD64_LOADI8_MEMINDEX:
			amd64_mov_reg_memindex_size (code, ins->dreg, ins->inst_basereg, 0, ins->inst_indexreg, 0, 8);
			break;
		case OP_LCONV_TO_I1:
		case OP_ICONV_TO_I1:
		case OP_SEXT_I1:
			amd64_widen_reg (code, ins->dreg, ins->sreg1, TRUE, FALSE);
			break;
		case OP_LCONV_TO_I2:
		case OP_ICONV_TO_I2:
		case OP_SEXT_I2:
			amd64_widen_reg (code, ins->dreg, ins->sreg1, TRUE, TRUE);
			break;
		case OP_LCONV_TO_U1:
		case OP_ICONV_TO_U1:
			amd64_widen_reg (code, ins->dreg, ins->sreg1, FALSE, FALSE);
			break;
		case OP_LCONV_TO_U2:
		case OP_ICONV_TO_U2:
			amd64_widen_reg (code, ins->dreg, ins->sreg1, FALSE, TRUE);
			break;
		case OP_ZEXT_I4:
			/* Clean out the upper word */
			amd64_mov_reg_reg (code, ins->dreg, ins->sreg1, 4);
			break;
		case OP_SEXT_I4:
			amd64_movsxd_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_COMPARE:
		case OP_LCOMPARE:
			amd64_alu_reg_reg (code, X86_CMP, ins->sreg1, ins->sreg2);
			break;
		case OP_COMPARE_IMM:
#if defined(MONO_ARCH_ILP32)
			/* Comparison of pointer immediates should be 4 bytes to avoid sign-extend problems */
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_reg_imm_size (code, X86_CMP, ins->sreg1, ins->inst_imm, 4);
			break;
#endif
		case OP_LCOMPARE_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_reg_imm (code, X86_CMP, ins->sreg1, ins->inst_imm);
			break;
		case OP_X86_COMPARE_REG_MEMBASE:
			amd64_alu_reg_membase (code, X86_CMP, ins->sreg1, ins->sreg2, ins->inst_offset);
			break;
		case OP_X86_TEST_NULL:
			amd64_test_reg_reg_size (code, ins->sreg1, ins->sreg1, 4);
			break;
		case OP_AMD64_TEST_NULL:
			amd64_test_reg_reg (code, ins->sreg1, ins->sreg1);
			break;

		case OP_X86_ADD_REG_MEMBASE:
			amd64_alu_reg_membase_size (code, X86_ADD, ins->sreg1, ins->sreg2, ins->inst_offset, 4);
			break;
		case OP_X86_SUB_REG_MEMBASE:
			amd64_alu_reg_membase_size (code, X86_SUB, ins->sreg1, ins->sreg2, ins->inst_offset, 4);
			break;
		case OP_X86_AND_REG_MEMBASE:
			amd64_alu_reg_membase_size (code, X86_AND, ins->sreg1, ins->sreg2, ins->inst_offset, 4);
			break;
		case OP_X86_OR_REG_MEMBASE:
			amd64_alu_reg_membase_size (code, X86_OR, ins->sreg1, ins->sreg2, ins->inst_offset, 4);
			break;
		case OP_X86_XOR_REG_MEMBASE:
			amd64_alu_reg_membase_size (code, X86_XOR, ins->sreg1, ins->sreg2, ins->inst_offset, 4);
			break;

		case OP_X86_ADD_MEMBASE_IMM:
			/* FIXME: Make a 64 version too */
			amd64_alu_membase_imm_size (code, X86_ADD, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 4);
			break;
		case OP_X86_SUB_MEMBASE_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_membase_imm_size (code, X86_SUB, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 4);
			break;
		case OP_X86_AND_MEMBASE_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_membase_imm_size (code, X86_AND, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 4);
			break;
		case OP_X86_OR_MEMBASE_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_membase_imm_size (code, X86_OR, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 4);
			break;
		case OP_X86_XOR_MEMBASE_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_membase_imm_size (code, X86_XOR, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 4);
			break;
		case OP_X86_ADD_MEMBASE_REG:
			amd64_alu_membase_reg_size (code, X86_ADD, ins->inst_basereg, ins->inst_offset, ins->sreg2, 4);
			break;
		case OP_X86_SUB_MEMBASE_REG:
			amd64_alu_membase_reg_size (code, X86_SUB, ins->inst_basereg, ins->inst_offset, ins->sreg2, 4);
			break;
		case OP_X86_AND_MEMBASE_REG:
			amd64_alu_membase_reg_size (code, X86_AND, ins->inst_basereg, ins->inst_offset, ins->sreg2, 4);
			break;
		case OP_X86_OR_MEMBASE_REG:
			amd64_alu_membase_reg_size (code, X86_OR, ins->inst_basereg, ins->inst_offset, ins->sreg2, 4);
			break;
		case OP_X86_XOR_MEMBASE_REG:
			amd64_alu_membase_reg_size (code, X86_XOR, ins->inst_basereg, ins->inst_offset, ins->sreg2, 4);
			break;
		case OP_X86_INC_MEMBASE:
			amd64_inc_membase_size (code, ins->inst_basereg, ins->inst_offset, 4);
			break;
		case OP_X86_INC_REG:
			amd64_inc_reg_size (code, ins->dreg, 4);
			break;
		case OP_X86_DEC_MEMBASE:
			amd64_dec_membase_size (code, ins->inst_basereg, ins->inst_offset, 4);
			break;
		case OP_X86_DEC_REG:
			amd64_dec_reg_size (code, ins->dreg, 4);
			break;
		case OP_X86_MUL_REG_MEMBASE:
		case OP_X86_MUL_MEMBASE_REG:
			amd64_imul_reg_membase_size (code, ins->sreg1, ins->sreg2, ins->inst_offset, 4);
			break;
		case OP_AMD64_ICOMPARE_MEMBASE_REG:
			amd64_alu_membase_reg_size (code, X86_CMP, ins->inst_basereg, ins->inst_offset, ins->sreg2, 4);
			break;
		case OP_AMD64_ICOMPARE_MEMBASE_IMM:
			amd64_alu_membase_imm_size (code, X86_CMP, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 4);
			break;
		case OP_AMD64_COMPARE_MEMBASE_REG:
			amd64_alu_membase_reg_size (code, X86_CMP, ins->inst_basereg, ins->inst_offset, ins->sreg2, 8);
			break;
		case OP_AMD64_COMPARE_MEMBASE_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_membase_imm_size (code, X86_CMP, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 8);
			break;
		case OP_X86_COMPARE_MEMBASE8_IMM:
			amd64_alu_membase8_imm_size (code, X86_CMP, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 4);
			break;
		case OP_AMD64_ICOMPARE_REG_MEMBASE:
			amd64_alu_reg_membase_size (code, X86_CMP, ins->sreg1, ins->sreg2, ins->inst_offset, 4);
			break;
		case OP_AMD64_COMPARE_REG_MEMBASE:
			amd64_alu_reg_membase_size (code, X86_CMP, ins->sreg1, ins->sreg2, ins->inst_offset, 8);
			break;

		case OP_AMD64_ADD_REG_MEMBASE:
			amd64_alu_reg_membase_size (code, X86_ADD, ins->sreg1, ins->sreg2, ins->inst_offset, 8);
			break;
		case OP_AMD64_SUB_REG_MEMBASE:
			amd64_alu_reg_membase_size (code, X86_SUB, ins->sreg1, ins->sreg2, ins->inst_offset, 8);
			break;
		case OP_AMD64_AND_REG_MEMBASE:
			amd64_alu_reg_membase_size (code, X86_AND, ins->sreg1, ins->sreg2, ins->inst_offset, 8);
			break;
		case OP_AMD64_OR_REG_MEMBASE:
			amd64_alu_reg_membase_size (code, X86_OR, ins->sreg1, ins->sreg2, ins->inst_offset, 8);
			break;
		case OP_AMD64_XOR_REG_MEMBASE:
			amd64_alu_reg_membase_size (code, X86_XOR, ins->sreg1, ins->sreg2, ins->inst_offset, 8);
			break;

		case OP_AMD64_ADD_MEMBASE_REG:
			amd64_alu_membase_reg_size (code, X86_ADD, ins->inst_basereg, ins->inst_offset, ins->sreg2, 8);
			break;
		case OP_AMD64_SUB_MEMBASE_REG:
			amd64_alu_membase_reg_size (code, X86_SUB, ins->inst_basereg, ins->inst_offset, ins->sreg2, 8);
			break;
		case OP_AMD64_AND_MEMBASE_REG:
			amd64_alu_membase_reg_size (code, X86_AND, ins->inst_basereg, ins->inst_offset, ins->sreg2, 8);
			break;
		case OP_AMD64_OR_MEMBASE_REG:
			amd64_alu_membase_reg_size (code, X86_OR, ins->inst_basereg, ins->inst_offset, ins->sreg2, 8);
			break;
		case OP_AMD64_XOR_MEMBASE_REG:
			amd64_alu_membase_reg_size (code, X86_XOR, ins->inst_basereg, ins->inst_offset, ins->sreg2, 8);
			break;

		case OP_AMD64_ADD_MEMBASE_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_membase_imm_size (code, X86_ADD, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 8);
			break;
		case OP_AMD64_SUB_MEMBASE_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_membase_imm_size (code, X86_SUB, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 8);
			break;
		case OP_AMD64_AND_MEMBASE_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_membase_imm_size (code, X86_AND, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 8);
			break;
		case OP_AMD64_OR_MEMBASE_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_membase_imm_size (code, X86_OR, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 8);
			break;
		case OP_AMD64_XOR_MEMBASE_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_membase_imm_size (code, X86_XOR, ins->inst_basereg, ins->inst_offset, ins->inst_imm, 8);
			break;

		case OP_BREAK:
			amd64_breakpoint (code);
			break;
		case OP_RELAXED_NOP:
			x86_prefix (code, X86_REP_PREFIX);
			x86_nop (code);
			break;
		case OP_HARD_NOP:
			x86_nop (code);
			break;
		case OP_NOP:
		case OP_DUMMY_USE:
		case OP_DUMMY_ICONST:
		case OP_DUMMY_I8CONST:
		case OP_DUMMY_R8CONST:
		case OP_DUMMY_R4CONST:
		case OP_NOT_REACHED:
		case OP_NOT_NULL:
			break;
		case OP_IL_SEQ_POINT:
			mono_add_seq_point (cfg, bb, ins, code - cfg->native_code);
			break;
		case OP_SEQ_POINT: {
			if (ins->flags & MONO_INST_SINGLE_STEP_LOC) {
				MonoInst *var = cfg->arch.ss_tramp_var;
				guint8 *label;

				/* Load ss_tramp_var */
				/* This is equal to &mono_amd64_ss_trampoline */
				amd64_mov_reg_membase (code, AMD64_R11, var->inst_basereg, var->inst_offset, 8);
				/* Load the trampoline address */
				amd64_mov_reg_membase (code, AMD64_R11, AMD64_R11, 0, 8);
				/* Call it if it is non-null */
				amd64_test_reg_reg (code, AMD64_R11, AMD64_R11);
				label = code;
				amd64_branch8 (code, X86_CC_Z, 0, FALSE);
				amd64_call_reg (code, AMD64_R11);
				amd64_patch (label, code);
			}

			/* 
			 * This is the address which is saved in seq points, 
			 */
			mono_add_seq_point (cfg, bb, ins, code - cfg->native_code);

			if (cfg->compile_aot) {
				const guint32 offset = code - cfg->native_code;
				guint32 val;
				MonoInst *info_var = cfg->arch.seq_point_info_var;
				guint8 *label;

				/* Load info var */
				amd64_mov_reg_membase (code, AMD64_R11, info_var->inst_basereg, info_var->inst_offset, 8);
				val = ((offset) * sizeof (target_mgreg_t)) + MONO_STRUCT_OFFSET (SeqPointInfo, bp_addrs);
				/* Load the info->bp_addrs [offset], which is either NULL or the address of the breakpoint trampoline */
				amd64_mov_reg_membase (code, AMD64_R11, AMD64_R11, val, 8);
				amd64_test_reg_reg (code, AMD64_R11, AMD64_R11);
				label = code;
				amd64_branch8 (code, X86_CC_Z, 0, FALSE);
				/* Call the trampoline */
				amd64_call_reg (code, AMD64_R11);
				amd64_patch (label, code);
			} else {
				MonoInst *var = cfg->arch.bp_tramp_var;
				guint8 *label;

				/*
				 * Emit a test+branch against a constant, the constant will be overwritten
				 * by mono_arch_set_breakpoint () to cause the test to fail.
				 */
				amd64_mov_reg_imm (code, AMD64_R11, 0);
				amd64_test_reg_reg (code, AMD64_R11, AMD64_R11);
				label = code;
				amd64_branch8 (code, X86_CC_Z, 0, FALSE);

				g_assert (var);
				g_assert (var->opcode == OP_REGOFFSET);
				/* Load bp_tramp_var */
				/* This is equal to &mono_amd64_bp_trampoline */
				amd64_mov_reg_membase (code, AMD64_R11, var->inst_basereg, var->inst_offset, 8);
				/* Call the trampoline */
				amd64_call_membase (code, AMD64_R11, 0);
				amd64_patch (label, code);
			}
			/*
			 * Add an additional nop so skipping the bp doesn't cause the ip to point
			 * to another IL offset.
			 */
			x86_nop (code);
			break;
		}
		case OP_ADDCC:
		case OP_LADDCC:
		case OP_LADD:
			amd64_alu_reg_reg (code, X86_ADD, ins->sreg1, ins->sreg2);
			break;
		case OP_ADC:
			amd64_alu_reg_reg (code, X86_ADC, ins->sreg1, ins->sreg2);
			break;
		case OP_ADD_IMM:
		case OP_LADD_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_reg_imm (code, X86_ADD, ins->dreg, ins->inst_imm);
			break;
		case OP_ADC_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_reg_imm (code, X86_ADC, ins->dreg, ins->inst_imm);
			break;
		case OP_SUBCC:
		case OP_LSUBCC:
		case OP_LSUB:
			amd64_alu_reg_reg (code, X86_SUB, ins->sreg1, ins->sreg2);
			break;
		case OP_SBB:
			amd64_alu_reg_reg (code, X86_SBB, ins->sreg1, ins->sreg2);
			break;
		case OP_SUB_IMM:
		case OP_LSUB_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_reg_imm (code, X86_SUB, ins->dreg, ins->inst_imm);
			break;
		case OP_SBB_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_reg_imm (code, X86_SBB, ins->dreg, ins->inst_imm);
			break;
		case OP_LAND:
			amd64_alu_reg_reg (code, X86_AND, ins->sreg1, ins->sreg2);
			break;
		case OP_AND_IMM:
		case OP_LAND_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_reg_imm (code, X86_AND, ins->sreg1, ins->inst_imm);
			break;
		case OP_LMUL:
			amd64_imul_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_MUL_IMM:
		case OP_LMUL_IMM:
		case OP_IMUL_IMM: {
			guint32 size = (ins->opcode == OP_IMUL_IMM) ? 4 : 8;
			
			switch (ins->inst_imm) {
			case 2:
				/* MOV r1, r2 */
				/* ADD r1, r1 */
				if (ins->dreg != ins->sreg1)
					amd64_mov_reg_reg (code, ins->dreg, ins->sreg1, size);
				amd64_alu_reg_reg (code, X86_ADD, ins->dreg, ins->dreg);
				break;
			case 3:
				/* LEA r1, [r2 + r2*2] */
				amd64_lea_memindex (code, ins->dreg, ins->sreg1, 0, ins->sreg1, 1);
				break;
			case 5:
				/* LEA r1, [r2 + r2*4] */
				amd64_lea_memindex (code, ins->dreg, ins->sreg1, 0, ins->sreg1, 2);
				break;
			case 6:
				/* LEA r1, [r2 + r2*2] */
				/* ADD r1, r1          */
				amd64_lea_memindex (code, ins->dreg, ins->sreg1, 0, ins->sreg1, 1);
				amd64_alu_reg_reg (code, X86_ADD, ins->dreg, ins->dreg);
				break;
			case 9:
				/* LEA r1, [r2 + r2*8] */
				amd64_lea_memindex (code, ins->dreg, ins->sreg1, 0, ins->sreg1, 3);
				break;
			case 10:
				/* LEA r1, [r2 + r2*4] */
				/* ADD r1, r1          */
				amd64_lea_memindex (code, ins->dreg, ins->sreg1, 0, ins->sreg1, 2);
				amd64_alu_reg_reg (code, X86_ADD, ins->dreg, ins->dreg);
				break;
			case 12:
				/* LEA r1, [r2 + r2*2] */
				/* SHL r1, 2           */
				amd64_lea_memindex (code, ins->dreg, ins->sreg1, 0, ins->sreg1, 1);
				amd64_shift_reg_imm (code, X86_SHL, ins->dreg, 2);
				break;
			case 25:
				/* LEA r1, [r2 + r2*4] */
				/* LEA r1, [r1 + r1*4] */
				amd64_lea_memindex (code, ins->dreg, ins->sreg1, 0, ins->sreg1, 2);
				amd64_lea_memindex (code, ins->dreg, ins->dreg, 0, ins->dreg, 2);
				break;
			case 100:
				/* LEA r1, [r2 + r2*4] */
				/* SHL r1, 2           */
				/* LEA r1, [r1 + r1*4] */
				amd64_lea_memindex (code, ins->dreg, ins->sreg1, 0, ins->sreg1, 2);
				amd64_shift_reg_imm (code, X86_SHL, ins->dreg, 2);
				amd64_lea_memindex (code, ins->dreg, ins->dreg, 0, ins->dreg, 2);
				break;
			default:
				amd64_imul_reg_reg_imm_size (code, ins->dreg, ins->sreg1, ins->inst_imm, size);
				break;
			}
			break;
		}
		case OP_LDIV:
		case OP_LREM:
			/* Regalloc magic makes the div/rem cases the same */
			if (ins->sreg2 == AMD64_RDX) {
				amd64_mov_membase_reg (code, AMD64_RSP, -8, AMD64_RDX, 8);
				amd64_cdq (code);
				amd64_div_membase (code, AMD64_RSP, -8, TRUE);
			} else {
				amd64_cdq (code);
				amd64_div_reg (code, ins->sreg2, TRUE);
			}
			break;
		case OP_LDIV_UN:
		case OP_LREM_UN:
			if (ins->sreg2 == AMD64_RDX) {
				amd64_mov_membase_reg (code, AMD64_RSP, -8, AMD64_RDX, 8);
				amd64_alu_reg_reg (code, X86_XOR, AMD64_RDX, AMD64_RDX);
				amd64_div_membase (code, AMD64_RSP, -8, FALSE);
			} else {
				amd64_alu_reg_reg (code, X86_XOR, AMD64_RDX, AMD64_RDX);
				amd64_div_reg (code, ins->sreg2, FALSE);
			}
			break;
		case OP_IDIV:
		case OP_IREM:
			if (ins->sreg2 == AMD64_RDX) {
				amd64_mov_membase_reg (code, AMD64_RSP, -8, AMD64_RDX, 8);
				amd64_cdq_size (code, 4);
				amd64_div_membase_size (code, AMD64_RSP, -8, TRUE, 4);
			} else {
				amd64_cdq_size (code, 4);
				amd64_div_reg_size (code, ins->sreg2, TRUE, 4);
			}
			break;
		case OP_IDIV_UN:
		case OP_IREM_UN:
			if (ins->sreg2 == AMD64_RDX) {
				amd64_mov_membase_reg (code, AMD64_RSP, -8, AMD64_RDX, 8);
				amd64_alu_reg_reg (code, X86_XOR, AMD64_RDX, AMD64_RDX);
				amd64_div_membase_size (code, AMD64_RSP, -8, FALSE, 4);
			} else {
				amd64_alu_reg_reg (code, X86_XOR, AMD64_RDX, AMD64_RDX);
				amd64_div_reg_size (code, ins->sreg2, FALSE, 4);
			}
			break;
		case OP_LMUL_OVF:
			amd64_imul_reg_reg (code, ins->sreg1, ins->sreg2);
			EMIT_COND_SYSTEM_EXCEPTION (X86_CC_O, FALSE, "OverflowException");
			break;
		case OP_LOR:
			amd64_alu_reg_reg (code, X86_OR, ins->sreg1, ins->sreg2);
			break;
		case OP_OR_IMM:
		case OP_LOR_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_reg_imm (code, X86_OR, ins->sreg1, ins->inst_imm);
			break;
		case OP_LXOR:
			amd64_alu_reg_reg (code, X86_XOR, ins->sreg1, ins->sreg2);
			break;
		case OP_XOR_IMM:
		case OP_LXOR_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_alu_reg_imm (code, X86_XOR, ins->sreg1, ins->inst_imm);
			break;
		case OP_LSHL:
			g_assert (ins->sreg2 == AMD64_RCX);
			amd64_shift_reg (code, X86_SHL, ins->dreg);
			break;
		case OP_LSHR:
			g_assert (ins->sreg2 == AMD64_RCX);
			amd64_shift_reg (code, X86_SAR, ins->dreg);
			break;
		case OP_SHR_IMM:
		case OP_LSHR_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_shift_reg_imm (code, X86_SAR, ins->dreg, ins->inst_imm);
			break;
		case OP_SHR_UN_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_shift_reg_imm_size (code, X86_SHR, ins->dreg, ins->inst_imm, 4);
			break;
		case OP_LSHR_UN_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_shift_reg_imm (code, X86_SHR, ins->dreg, ins->inst_imm);
			break;
		case OP_LSHR_UN:
			g_assert (ins->sreg2 == AMD64_RCX);
			amd64_shift_reg (code, X86_SHR, ins->dreg);
			break;
		case OP_SHL_IMM:
		case OP_LSHL_IMM:
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_shift_reg_imm (code, X86_SHL, ins->dreg, ins->inst_imm);
			break;

		case OP_IADDCC:
		case OP_IADD:
			amd64_alu_reg_reg_size (code, X86_ADD, ins->sreg1, ins->sreg2, 4);
			break;
		case OP_IADC:
			amd64_alu_reg_reg_size (code, X86_ADC, ins->sreg1, ins->sreg2, 4);
			break;
		case OP_IADD_IMM:
			amd64_alu_reg_imm_size (code, X86_ADD, ins->dreg, ins->inst_imm, 4);
			break;
		case OP_IADC_IMM:
			amd64_alu_reg_imm_size (code, X86_ADC, ins->dreg, ins->inst_imm, 4);
			break;
		case OP_ISUBCC:
		case OP_ISUB:
			amd64_alu_reg_reg_size (code, X86_SUB, ins->sreg1, ins->sreg2, 4);
			break;
		case OP_ISBB:
			amd64_alu_reg_reg_size (code, X86_SBB, ins->sreg1, ins->sreg2, 4);
			break;
		case OP_ISUB_IMM:
			amd64_alu_reg_imm_size (code, X86_SUB, ins->dreg, ins->inst_imm, 4);
			break;
		case OP_ISBB_IMM:
			amd64_alu_reg_imm_size (code, X86_SBB, ins->dreg, ins->inst_imm, 4);
			break;
		case OP_IAND:
			amd64_alu_reg_reg_size (code, X86_AND, ins->sreg1, ins->sreg2, 4);
			break;
		case OP_IAND_IMM:
			amd64_alu_reg_imm_size (code, X86_AND, ins->sreg1, ins->inst_imm, 4);
			break;
		case OP_IOR:
			amd64_alu_reg_reg_size (code, X86_OR, ins->sreg1, ins->sreg2, 4);
			break;
		case OP_IOR_IMM:
			amd64_alu_reg_imm_size (code, X86_OR, ins->sreg1, ins->inst_imm, 4);
			break;
		case OP_IXOR:
			amd64_alu_reg_reg_size (code, X86_XOR, ins->sreg1, ins->sreg2, 4);
			break;
		case OP_IXOR_IMM:
			amd64_alu_reg_imm_size (code, X86_XOR, ins->sreg1, ins->inst_imm, 4);
			break;
		case OP_INEG:
			amd64_neg_reg_size (code, ins->sreg1, 4);
			break;
		case OP_INOT:
			amd64_not_reg_size (code, ins->sreg1, 4);
			break;
		case OP_ISHL:
			g_assert (ins->sreg2 == AMD64_RCX);
			amd64_shift_reg_size (code, X86_SHL, ins->dreg, 4);
			break;
		case OP_ISHR:
			g_assert (ins->sreg2 == AMD64_RCX);
			amd64_shift_reg_size (code, X86_SAR, ins->dreg, 4);
			break;
		case OP_ISHR_IMM:
			amd64_shift_reg_imm_size (code, X86_SAR, ins->dreg, ins->inst_imm, 4);
			break;
		case OP_ISHR_UN_IMM:
			amd64_shift_reg_imm_size (code, X86_SHR, ins->dreg, ins->inst_imm, 4);
			break;
		case OP_ISHR_UN:
			g_assert (ins->sreg2 == AMD64_RCX);
			amd64_shift_reg_size (code, X86_SHR, ins->dreg, 4);
			break;
		case OP_ISHL_IMM:
			amd64_shift_reg_imm_size (code, X86_SHL, ins->dreg, ins->inst_imm, 4);
			break;
		case OP_IMUL:
			amd64_imul_reg_reg_size (code, ins->sreg1, ins->sreg2, 4);
			break;
		case OP_IMUL_OVF:
			amd64_imul_reg_reg_size (code, ins->sreg1, ins->sreg2, 4);
			EMIT_COND_SYSTEM_EXCEPTION (X86_CC_O, FALSE, "OverflowException");
			break;
		case OP_IMUL_OVF_UN:
		case OP_LMUL_OVF_UN: {
			/* the mul operation and the exception check should most likely be split */
			int non_eax_reg, saved_eax = FALSE, saved_edx = FALSE;
			int size = (ins->opcode == OP_IMUL_OVF_UN) ? 4 : 8;
			/*g_assert (ins->sreg2 == X86_EAX);
			g_assert (ins->dreg == X86_EAX);*/
			if (ins->sreg2 == X86_EAX) {
				non_eax_reg = ins->sreg1;
			} else if (ins->sreg1 == X86_EAX) {
				non_eax_reg = ins->sreg2;
			} else {
				/* no need to save since we're going to store to it anyway */
				if (ins->dreg != X86_EAX) {
					saved_eax = TRUE;
					amd64_push_reg (code, X86_EAX);
				}
				amd64_mov_reg_reg (code, X86_EAX, ins->sreg1, size);
				non_eax_reg = ins->sreg2;
			}
			if (ins->dreg == X86_EDX) {
				if (!saved_eax) {
					saved_eax = TRUE;
					amd64_push_reg (code, X86_EAX);
				}
			} else {
				saved_edx = TRUE;
				amd64_push_reg (code, X86_EDX);
			}
			amd64_mul_reg_size (code, non_eax_reg, FALSE, size);
			/* save before the check since pop and mov don't change the flags */
			if (ins->dreg != X86_EAX)
				amd64_mov_reg_reg (code, ins->dreg, X86_EAX, size);
			if (saved_edx)
				amd64_pop_reg (code, X86_EDX);
			if (saved_eax)
				amd64_pop_reg (code, X86_EAX);
			EMIT_COND_SYSTEM_EXCEPTION (X86_CC_O, FALSE, "OverflowException");
			break;
		}
		case OP_ICOMPARE:
			amd64_alu_reg_reg_size (code, X86_CMP, ins->sreg1, ins->sreg2, 4);
			break;
		case OP_ICOMPARE_IMM:
			amd64_alu_reg_imm_size (code, X86_CMP, ins->sreg1, ins->inst_imm, 4);
			break;
		case OP_IBEQ:
		case OP_IBLT:
		case OP_IBGT:
		case OP_IBGE:
		case OP_IBLE:
		case OP_LBEQ:
		case OP_LBLT:
		case OP_LBGT:
		case OP_LBGE:
		case OP_LBLE:
		case OP_IBNE_UN:
		case OP_IBLT_UN:
		case OP_IBGT_UN:
		case OP_IBGE_UN:
		case OP_IBLE_UN:
		case OP_LBNE_UN:
		case OP_LBLT_UN:
		case OP_LBGT_UN:
		case OP_LBGE_UN:
		case OP_LBLE_UN:
			EMIT_COND_BRANCH (ins, cc_table [mono_opcode_to_cond (ins->opcode)], cc_signed_table [mono_opcode_to_cond (ins->opcode)]);
			break;

		case OP_CMOV_IEQ:
		case OP_CMOV_IGE:
		case OP_CMOV_IGT:
		case OP_CMOV_ILE:
		case OP_CMOV_ILT:
		case OP_CMOV_INE_UN:
		case OP_CMOV_IGE_UN:
		case OP_CMOV_IGT_UN:
		case OP_CMOV_ILE_UN:
		case OP_CMOV_ILT_UN:
		case OP_CMOV_LEQ:
		case OP_CMOV_LGE:
		case OP_CMOV_LGT:
		case OP_CMOV_LLE:
		case OP_CMOV_LLT:
		case OP_CMOV_LNE_UN:
		case OP_CMOV_LGE_UN:
		case OP_CMOV_LGT_UN:
		case OP_CMOV_LLE_UN:
		case OP_CMOV_LLT_UN:
			g_assert (ins->dreg == ins->sreg1);
			/* This needs to operate on 64 bit values */
			amd64_cmov_reg (code, cc_table [mono_opcode_to_cond (ins->opcode)], cc_signed_table [mono_opcode_to_cond (ins->opcode)], ins->dreg, ins->sreg2);
			break;

		case OP_LNOT:
			amd64_not_reg (code, ins->sreg1);
			break;
		case OP_LNEG:
			amd64_neg_reg (code, ins->sreg1);
			break;

		case OP_ICONST:
		case OP_I8CONST:
			if ((((guint64)ins->inst_c0) >> 32) == 0 && !mini_debug_options.single_imm_size)
				amd64_mov_reg_imm_size (code, ins->dreg, ins->inst_c0, 4);
			else
				amd64_mov_reg_imm_size (code, ins->dreg, ins->inst_c0, 8);
			break;
		case OP_AOTCONST:
			mono_add_patch_info (cfg, offset, (MonoJumpInfoType)(gsize)ins->inst_i1, ins->inst_p0);
			amd64_mov_reg_membase (code, ins->dreg, AMD64_RIP, 0, sizeof(gpointer));
			break;
		case OP_JUMP_TABLE:
			mono_add_patch_info (cfg, offset, (MonoJumpInfoType)(gsize)ins->inst_i1, ins->inst_p0);
			amd64_mov_reg_imm_size (code, ins->dreg, 0, 8);
			break;
		case OP_MOVE:
			if (ins->dreg != ins->sreg1)
				amd64_mov_reg_reg (code, ins->dreg, ins->sreg1, sizeof (target_mgreg_t));
			break;
		case OP_AMD64_SET_XMMREG_R4: {
			if (cfg->r4fp) {
				if (ins->dreg != ins->sreg1)
					amd64_sse_movss_reg_reg (code, ins->dreg, ins->sreg1);
			} else {
				amd64_sse_cvtsd2ss_reg_reg (code, ins->dreg, ins->sreg1);
			}
			break;
		}
		case OP_AMD64_SET_XMMREG_R8: {
			if (ins->dreg != ins->sreg1)
				amd64_sse_movsd_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		}

		case OP_TAILCALL_PARAMETER:
			// This opcode helps compute sizes, i.e.
			// of the subsequent OP_TAILCALL, but contributes no code.
			g_assert (ins->next);
			break;

		case OP_TAILCALL:
		case OP_TAILCALL_REG:
		case OP_TAILCALL_MEMBASE: {
			call = (MonoCallInst*)ins;
			int i, save_area_offset;
			gboolean tailcall_membase = (ins->opcode == OP_TAILCALL_MEMBASE);
			gboolean tailcall_reg = (ins->opcode == OP_TAILCALL_REG);

			g_assert (!cfg->method->save_lmf);

			max_len += AMD64_NREG * 4;
			max_len += call->stack_usage / sizeof (target_mgreg_t) * ins_get_size (OP_TAILCALL_PARAMETER);
			code = realloc_code (cfg, max_len);

			// FIXME hardcoding RAX here is not ideal.

			if (tailcall_reg) {
				int const reg = ins->sreg1;
				g_assert (reg > -1);
				if (reg != AMD64_RAX)
					amd64_mov_reg_reg (code, AMD64_RAX, reg, 8);
			} else if (tailcall_membase) {
				int const reg = ins->sreg1;
				g_assert (reg > -1);
				amd64_mov_reg_membase (code, AMD64_RAX, reg, ins->inst_offset, 8);
			} else {
				 if (cfg->compile_aot) {
					mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_METHOD_JUMP, call->method);
					amd64_mov_reg_membase (code, AMD64_RAX, AMD64_RIP, 0, 8);
				} else {
					// FIXME Patch data instead of code.
					guint32 pad_size = (guint32)((code + 2 - cfg->native_code) % 8);
					if (pad_size)
						amd64_padding (code, 8 - pad_size);
					mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_METHOD_JUMP, call->method);
					amd64_set_reg_template (code, AMD64_RAX);
				}
			}

			/* Restore callee saved registers */
			save_area_offset = cfg->arch.reg_save_area_offset;
			for (i = 0; i < AMD64_NREG; ++i)
				if (AMD64_IS_CALLEE_SAVED_REG (i) && (cfg->used_int_regs & ((regmask_t)1 << i))) {
					amd64_mov_reg_membase (code, i, cfg->frame_reg, save_area_offset, 8);
					save_area_offset += 8;
				}

			if (cfg->arch.omit_fp) {
				if (cfg->arch.stack_alloc_size)
					amd64_alu_reg_imm (code, X86_ADD, AMD64_RSP, cfg->arch.stack_alloc_size);
				// FIXME:
				if (call->stack_usage)
					NOT_IMPLEMENTED;
			} else {
				amd64_push_reg (code, AMD64_RAX);
				/* Copy arguments on the stack to our argument area */
				// FIXME use rep mov for constant code size, before nonvolatiles
				// restored, first saving rsi, rdi into volatiles
				for (i = 0; i < call->stack_usage; i += sizeof (target_mgreg_t)) {
					amd64_mov_reg_membase (code, AMD64_RAX, AMD64_RSP, i + 8, sizeof (target_mgreg_t));
					amd64_mov_membase_reg (code, AMD64_RBP, ARGS_OFFSET + i, AMD64_RAX, sizeof (target_mgreg_t));
				}
				amd64_pop_reg (code, AMD64_RAX);
#ifdef TARGET_WIN32
				amd64_lea_membase (code, AMD64_RSP, AMD64_RBP, 0);
				amd64_pop_reg (code, AMD64_RBP);
				mono_emit_unwind_op_same_value (cfg, code, AMD64_RBP);
#else
				amd64_leave (code);
#endif
			}

#ifdef TARGET_WIN32
			// Redundant REX byte indicates a tailcall to the native unwinder. It means nothing to the processor.
			// https://github.com/dotnet/coreclr/blob/966dabb5bb3c4bf1ea885e1e8dc6528e8c64dc4f/src/unwinder/amd64/unwinder_amd64.cpp#L1394
			// FIXME This should be jmp rip+32 for AOT direct to same assembly.
			// FIXME This should be jmp [rip+32] for AOT direct to not-same assembly (through data).
			// FIXME This should be jmp [rip+32] for JIT direct -- patch data instead of code.
			// This is only close to ideal for tailcall_membase, and even then it should
			// have a more dynamic register allocation.
			x86_imm_emit8 (code, 0x48);
			amd64_jump_reg (code, AMD64_RAX);
#else
			// NT does not have varargs rax use, and NT ABI does not have red zone.
			// Use red-zone mov/jmp instead of push/ret to preserve call/ret speculation stack.
			// FIXME Just like NT the direct cases are are not ideal.
			amd64_mov_membase_reg (code, AMD64_RSP, -8, AMD64_RAX, 8);
			code = amd64_handle_varargs_call (cfg, code, call, FALSE);
			amd64_jump_membase (code, AMD64_RSP, -8);
#endif
			ins->flags |= MONO_INST_GC_CALLSITE;
			ins->backend.pc_offset = code - cfg->native_code;
			break;
		}
		case OP_CHECK_THIS:
			/* ensure ins->sreg1 is not NULL */
			amd64_alu_membase_imm_size (code, X86_CMP, ins->sreg1, 0, 0, 4);
			break;
		case OP_ARGLIST: {
			amd64_lea_membase (code, AMD64_R11, cfg->frame_reg, cfg->sig_cookie);
			amd64_mov_membase_reg (code, ins->sreg1, 0, AMD64_R11, sizeof(gpointer));
			break;
		}
		case OP_CALL:
		case OP_FCALL:
		case OP_RCALL:
		case OP_LCALL:
		case OP_VCALL:
		case OP_VCALL2:
		case OP_VOIDCALL:
			call = (MonoCallInst*)ins;

			code = amd64_handle_varargs_call (cfg, code, call, FALSE);
			code = emit_call (cfg, call, code, MONO_JIT_ICALL_ZeroIsReserved);
			ins->flags |= MONO_INST_GC_CALLSITE;
			ins->backend.pc_offset = code - cfg->native_code;
			code = emit_move_return_value (cfg, ins, code);
			break;
		case OP_FCALL_REG:
		case OP_RCALL_REG:
		case OP_LCALL_REG:
		case OP_VCALL_REG:
		case OP_VCALL2_REG:
		case OP_VOIDCALL_REG:
		case OP_CALL_REG:
			call = (MonoCallInst*)ins;

			if (AMD64_IS_ARGUMENT_REG (ins->sreg1)) {
				amd64_mov_reg_reg (code, AMD64_R11, ins->sreg1, 8);
				ins->sreg1 = AMD64_R11;
			}

			code = amd64_handle_varargs_call (cfg, code, call, TRUE);
			amd64_call_reg (code, ins->sreg1);
			ins->flags |= MONO_INST_GC_CALLSITE;
			ins->backend.pc_offset = code - cfg->native_code;
			code = emit_move_return_value (cfg, ins, code);
			break;
		case OP_FCALL_MEMBASE:
		case OP_RCALL_MEMBASE:
		case OP_LCALL_MEMBASE:
		case OP_VCALL_MEMBASE:
		case OP_VCALL2_MEMBASE:
		case OP_VOIDCALL_MEMBASE:
		case OP_CALL_MEMBASE:
			call = (MonoCallInst*)ins;

			amd64_call_membase (code, ins->sreg1, ins->inst_offset);
			ins->flags |= MONO_INST_GC_CALLSITE;
			ins->backend.pc_offset = code - cfg->native_code;
			code = emit_move_return_value (cfg, ins, code);
			break;
		case OP_DYN_CALL: {
			int i, limit_reg, index_reg, src_reg, dst_reg;
			MonoInst *var = cfg->dyn_call_var;
			guint8 *label;
			guint8 *buf [16];

			g_assert (var->opcode == OP_REGOFFSET);

			/* r11 = args buffer filled by mono_arch_get_dyn_call_args () */
			amd64_mov_reg_reg (code, AMD64_R11, ins->sreg1, 8);
			/* r10 = ftn */
			amd64_mov_reg_reg (code, AMD64_R10, ins->sreg2, 8);

			/* Save args buffer */
			amd64_mov_membase_reg (code, var->inst_basereg, var->inst_offset, AMD64_R11, 8);

			/* Set fp arg regs */
			amd64_mov_reg_membase (code, AMD64_RAX, AMD64_R11, MONO_STRUCT_OFFSET (DynCallArgs, has_fp), sizeof (target_mgreg_t));
			amd64_test_reg_reg (code, AMD64_RAX, AMD64_RAX);
			label = code;
			amd64_branch8 (code, X86_CC_Z, -1, 1);
			for (i = 0; i < FLOAT_PARAM_REGS; ++i)
				amd64_sse_movsd_reg_membase (code, i, AMD64_R11, MONO_STRUCT_OFFSET (DynCallArgs, fregs) + (i * sizeof (double)));
			amd64_patch (label, code);

			/* Allocate param area */
			/* This doesn't need to be freed since OP_DYN_CALL is never called in a loop */
			amd64_mov_reg_membase (code, AMD64_RAX, AMD64_R11, MONO_STRUCT_OFFSET (DynCallArgs, nstack_args), 8);
			amd64_shift_reg_imm (code, X86_SHL, AMD64_RAX, 3);
			amd64_alu_reg_reg (code, X86_SUB, AMD64_RSP, AMD64_RAX);
			/* Set stack args */
			/* rax/rcx/rdx/r8/r9 is scratch */
			limit_reg = AMD64_RAX;
			index_reg = AMD64_RCX;
			src_reg = AMD64_R8;
			dst_reg = AMD64_R9;
			amd64_mov_reg_membase (code, limit_reg, AMD64_R11, MONO_STRUCT_OFFSET (DynCallArgs, nstack_args), 8);
			amd64_mov_reg_imm (code, index_reg, 0);
			amd64_lea_membase (code, src_reg, AMD64_R11, MONO_STRUCT_OFFSET (DynCallArgs, regs) + ((PARAM_REGS) * sizeof (target_mgreg_t)));
			amd64_mov_reg_reg (code, dst_reg, AMD64_RSP, 8);
			buf [0] = code;
			x86_jump8 (code, 0);
			buf [1] = code;
			amd64_mov_reg_membase (code, AMD64_RDX, src_reg, 0, 8);
			amd64_mov_membase_reg (code, dst_reg, 0, AMD64_RDX, 8);
			amd64_alu_reg_imm (code, X86_ADD, index_reg, 1);
			amd64_alu_reg_imm (code, X86_ADD, src_reg, 8);
			amd64_alu_reg_imm (code, X86_ADD, dst_reg, 8);
			amd64_patch (buf [0], code);
			amd64_alu_reg_reg (code, X86_CMP, index_reg, limit_reg);
			buf [2] = code;
			x86_branch8 (code, X86_CC_LT, 0, FALSE);
			amd64_patch (buf [2], buf [1]);

			/* Set argument registers */
			for (i = 0; i < PARAM_REGS; ++i)
				amd64_mov_reg_membase (code, param_regs [i], AMD64_R11, MONO_STRUCT_OFFSET (DynCallArgs, regs) + (i * sizeof (target_mgreg_t)), sizeof (target_mgreg_t));
			
			/* Make the call */
			amd64_call_reg (code, AMD64_R10);

			ins->flags |= MONO_INST_GC_CALLSITE;
			ins->backend.pc_offset = code - cfg->native_code;

			/* Save result */
			amd64_mov_reg_membase (code, AMD64_R11, var->inst_basereg, var->inst_offset, 8);
			amd64_mov_membase_reg (code, AMD64_R11, MONO_STRUCT_OFFSET (DynCallArgs, res), AMD64_RAX, 8);
			amd64_sse_movsd_membase_reg (code, AMD64_R11, MONO_STRUCT_OFFSET (DynCallArgs, fregs), AMD64_XMM0);
			amd64_sse_movsd_membase_reg (code, AMD64_R11, MONO_STRUCT_OFFSET (DynCallArgs, fregs) + sizeof (double), AMD64_XMM1);
			break;
		}
		case OP_AMD64_SAVE_SP_TO_LMF: {
			MonoInst *lmf_var = cfg->lmf_var;
			amd64_mov_membase_reg (code, lmf_var->inst_basereg, lmf_var->inst_offset + MONO_STRUCT_OFFSET (MonoLMF, rsp), AMD64_RSP, 8);
			break;
		}
		case OP_X86_PUSH:
			g_assert_not_reached ();
			amd64_push_reg (code, ins->sreg1);
			break;
		case OP_X86_PUSH_IMM:
			g_assert_not_reached ();
			g_assert (amd64_is_imm32 (ins->inst_imm));
			amd64_push_imm (code, ins->inst_imm);
			break;
		case OP_X86_PUSH_MEMBASE:
			g_assert_not_reached ();
			amd64_push_membase (code, ins->inst_basereg, ins->inst_offset);
			break;
		case OP_X86_PUSH_OBJ: {
			int size = ALIGN_TO (ins->inst_imm, 8);

			g_assert_not_reached ();

			amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, size);
			amd64_push_reg (code, AMD64_RDI);
			amd64_push_reg (code, AMD64_RSI);
			amd64_push_reg (code, AMD64_RCX);
			if (ins->inst_offset)
				amd64_lea_membase (code, AMD64_RSI, ins->inst_basereg, ins->inst_offset);
			else
				amd64_mov_reg_reg (code, AMD64_RSI, ins->inst_basereg, 8);
			amd64_lea_membase (code, AMD64_RDI, AMD64_RSP, (3 * 8));
			amd64_mov_reg_imm (code, AMD64_RCX, (size >> 3));
			amd64_cld (code);
			amd64_prefix (code, X86_REP_PREFIX);
			amd64_movsd (code);
			amd64_pop_reg (code, AMD64_RCX);
			amd64_pop_reg (code, AMD64_RSI);
			amd64_pop_reg (code, AMD64_RDI);
			break;
		}
		case OP_GENERIC_CLASS_INIT: {
			guint8 *jump;

			g_assert (ins->sreg1 == MONO_AMD64_ARG_REG1);

			amd64_test_membase_imm_size (code, ins->sreg1, MONO_STRUCT_OFFSET (MonoVTable, initialized), 1, 1);
			jump = code;
			amd64_branch8 (code, X86_CC_NZ, -1, 1);

			code = emit_call (cfg, NULL, code, MONO_JIT_ICALL_mono_generic_class_init);
			ins->flags |= MONO_INST_GC_CALLSITE;
			ins->backend.pc_offset = code - cfg->native_code;

			x86_patch (jump, code);
			break;
		}

		case OP_X86_LEA:
			amd64_lea_memindex (code, ins->dreg, ins->sreg1, ins->inst_imm, ins->sreg2, ins->backend.shift_amount);
			break;
		case OP_X86_LEA_MEMBASE:
			amd64_lea4_membase (code, ins->dreg, ins->sreg1, ins->inst_imm);
			break;
		case OP_AMD64_LEA_MEMBASE:
			amd64_lea_membase (code, ins->dreg, ins->sreg1, ins->inst_imm);
			break;
		case OP_X86_XCHG:
			amd64_xchg_reg_reg (code, ins->sreg1, ins->sreg2, 4);
			break;
		case OP_LOCALLOC:
			/* keep alignment */
			amd64_alu_reg_imm (code, X86_ADD, ins->sreg1, MONO_ARCH_FRAME_ALIGNMENT - 1);
			amd64_alu_reg_imm (code, X86_AND, ins->sreg1, ~(MONO_ARCH_FRAME_ALIGNMENT - 1));
			code = mono_emit_stack_alloc (cfg, code, ins);
			amd64_mov_reg_reg (code, ins->dreg, AMD64_RSP, 8);
			if (cfg->param_area)
				amd64_alu_reg_imm (code, X86_ADD, ins->dreg, cfg->param_area);
			break;
		case OP_LOCALLOC_IMM: {
			guint32 size = ins->inst_imm;
			size = (size + (MONO_ARCH_FRAME_ALIGNMENT - 1)) & ~ (MONO_ARCH_FRAME_ALIGNMENT - 1);

			if (ins->flags & MONO_INST_INIT) {
				if (size < 64) {
					int i;

					amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, size);
					amd64_alu_reg_reg (code, X86_XOR, ins->dreg, ins->dreg);

					for (i = 0; i < size; i += 8)
						amd64_mov_membase_reg (code, AMD64_RSP, i, ins->dreg, 8);
					amd64_mov_reg_reg (code, ins->dreg, AMD64_RSP, 8);					
				} else {
					amd64_mov_reg_imm (code, ins->dreg, size);
					ins->sreg1 = ins->dreg;

					code = mono_emit_stack_alloc (cfg, code, ins);
					amd64_mov_reg_reg (code, ins->dreg, AMD64_RSP, 8);
				}
			} else {
				amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, size);
				amd64_mov_reg_reg (code, ins->dreg, AMD64_RSP, 8);
			}
			if (cfg->param_area)
				amd64_alu_reg_imm (code, X86_ADD, ins->dreg, cfg->param_area);
			break;
		}
		case OP_THROW: {
			amd64_mov_reg_reg (code, AMD64_ARG_REG1, ins->sreg1, 8);
			code = emit_call (cfg, NULL, code, MONO_JIT_ICALL_mono_arch_throw_exception);
			ins->flags |= MONO_INST_GC_CALLSITE;
			ins->backend.pc_offset = code - cfg->native_code;
			break;
		}
		case OP_RETHROW: {
			amd64_mov_reg_reg (code, AMD64_ARG_REG1, ins->sreg1, 8);
			code = emit_call (cfg, NULL, code, MONO_JIT_ICALL_mono_arch_rethrow_exception);
			ins->flags |= MONO_INST_GC_CALLSITE;
			ins->backend.pc_offset = code - cfg->native_code;
			break;
		}
		case OP_CALL_HANDLER: 
			/* Align stack */
			amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, 8);
			mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_BB, ins->inst_target_bb);
			amd64_call_imm (code, 0);
			/*
			 * ins->inst_eh_blocks and bb->clause_holes are part of same GList.
			 * Holes from bb->clause_holes will be added separately for the entire
			 * basic block. Add only the rest of them.
			 */
			for (GList *tmp = ins->inst_eh_blocks; tmp != bb->clause_holes; tmp = tmp->prev)
				mono_cfg_add_try_hole (cfg, ((MonoLeaveClause *) tmp->data)->clause, code, bb);
			/* Restore stack alignment */
			amd64_alu_reg_imm (code, X86_ADD, AMD64_RSP, 8);
			break;
		case OP_START_HANDLER: {
			/* Even though we're saving RSP, use sizeof */
			/* gpointer because spvar is of type IntPtr */
			/* see: mono_create_spvar_for_region */
			MonoInst *spvar = mono_find_spvar_for_region (cfg, bb->region);
			amd64_mov_membase_reg (code, spvar->inst_basereg, spvar->inst_offset, AMD64_RSP, sizeof(gpointer));

			if ((MONO_BBLOCK_IS_IN_REGION (bb, MONO_REGION_FINALLY) ||
				 MONO_BBLOCK_IS_IN_REGION (bb, MONO_REGION_FILTER) ||
				 MONO_BBLOCK_IS_IN_REGION (bb, MONO_REGION_FAULT)) &&
				cfg->param_area) {
				amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, ALIGN_TO (cfg->param_area, MONO_ARCH_FRAME_ALIGNMENT));
			}
			break;
		}
		case OP_ENDFINALLY: {
			MonoInst *spvar = mono_find_spvar_for_region (cfg, bb->region);
			amd64_mov_reg_membase (code, AMD64_RSP, spvar->inst_basereg, spvar->inst_offset, sizeof(gpointer));
			amd64_ret (code);
			break;
		}
		case OP_ENDFILTER: {
			MonoInst *spvar = mono_find_spvar_for_region (cfg, bb->region);
			amd64_mov_reg_membase (code, AMD64_RSP, spvar->inst_basereg, spvar->inst_offset, sizeof(gpointer));
			/* The local allocator will put the result into RAX */
			amd64_ret (code);
			break;
		}
		case OP_GET_EX_OBJ:
			if (ins->dreg != AMD64_RAX)
				amd64_mov_reg_reg (code, ins->dreg, AMD64_RAX, sizeof (target_mgreg_t));
			break;
		case OP_LABEL:
			ins->inst_c0 = code - cfg->native_code;
			break;
		case OP_BR:
			//g_print ("target: %p, next: %p, curr: %p, last: %p\n", ins->inst_target_bb, bb->next_bb, ins, bb->last_ins);
			//if ((ins->inst_target_bb == bb->next_bb) && ins == bb->last_ins)
			//break;
				if (ins->inst_target_bb->native_offset) {
					amd64_jump_code (code, cfg->native_code + ins->inst_target_bb->native_offset); 
				} else {
					mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_BB, ins->inst_target_bb);
					if (optimize_branch_pred &&
					    x86_is_imm8 (ins->inst_target_bb->max_offset - offset))
						x86_jump8 (code, 0);
					else 
						x86_jump32 (code, 0);
			}
			break;
		case OP_BR_REG:
			amd64_jump_reg (code, ins->sreg1);
			break;
		case OP_ICNEQ:
		case OP_ICGE:
		case OP_ICLE:
		case OP_ICGE_UN:
		case OP_ICLE_UN:

		case OP_CEQ:
		case OP_LCEQ:
		case OP_ICEQ:
		case OP_CLT:
		case OP_LCLT:
		case OP_ICLT:
		case OP_CGT:
		case OP_ICGT:
		case OP_LCGT:
		case OP_CLT_UN:
		case OP_LCLT_UN:
		case OP_ICLT_UN:
		case OP_CGT_UN:
		case OP_LCGT_UN:
		case OP_ICGT_UN:
			amd64_set_reg (code, cc_table [mono_opcode_to_cond (ins->opcode)], ins->dreg, cc_signed_table [mono_opcode_to_cond (ins->opcode)]);
			amd64_widen_reg (code, ins->dreg, ins->dreg, FALSE, FALSE);
			break;
		case OP_COND_EXC_EQ:
		case OP_COND_EXC_NE_UN:
		case OP_COND_EXC_LT:
		case OP_COND_EXC_LT_UN:
		case OP_COND_EXC_GT:
		case OP_COND_EXC_GT_UN:
		case OP_COND_EXC_GE:
		case OP_COND_EXC_GE_UN:
		case OP_COND_EXC_LE:
		case OP_COND_EXC_LE_UN:
		case OP_COND_EXC_IEQ:
		case OP_COND_EXC_INE_UN:
		case OP_COND_EXC_ILT:
		case OP_COND_EXC_ILT_UN:
		case OP_COND_EXC_IGT:
		case OP_COND_EXC_IGT_UN:
		case OP_COND_EXC_IGE:
		case OP_COND_EXC_IGE_UN:
		case OP_COND_EXC_ILE:
		case OP_COND_EXC_ILE_UN:
			EMIT_COND_SYSTEM_EXCEPTION (cc_table [mono_opcode_to_cond (ins->opcode)], cc_signed_table [mono_opcode_to_cond (ins->opcode)], (const char *)ins->inst_p1);
			break;
		case OP_COND_EXC_OV:
		case OP_COND_EXC_NO:
		case OP_COND_EXC_C:
		case OP_COND_EXC_NC:
			EMIT_COND_SYSTEM_EXCEPTION (branch_cc_table [ins->opcode - OP_COND_EXC_EQ], 
						    (ins->opcode < OP_COND_EXC_NE_UN), (const char *)ins->inst_p1);
			break;
		case OP_COND_EXC_IOV:
		case OP_COND_EXC_INO:
		case OP_COND_EXC_IC:
		case OP_COND_EXC_INC:
			EMIT_COND_SYSTEM_EXCEPTION (branch_cc_table [ins->opcode - OP_COND_EXC_IEQ], 
						    (ins->opcode < OP_COND_EXC_INE_UN), (const char *)ins->inst_p1);
			break;

		/* floating point opcodes */
		case OP_R8CONST: {
			double d = *(double *)ins->inst_p0;

			if ((d == 0.0) && (mono_signbit (d) == 0)) {
				amd64_sse_xorpd_reg_reg (code, ins->dreg, ins->dreg);
			} else if (cfg->compile_aot && cfg->code_exec_only) {
				mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_R8_GOT, ins->inst_p0);
				amd64_mov_reg_membase (code, AMD64_R11, AMD64_RIP, 0, sizeof(gpointer));
				amd64_sse_movsd_reg_membase (code, ins->dreg, AMD64_R11, 0);
			} else {
				mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_R8, ins->inst_p0);
				amd64_sse_movsd_reg_membase (code, ins->dreg, AMD64_RIP, 0);
			}
			break;
		}
		case OP_R4CONST: {
			float f = *(float *)ins->inst_p0;

			if ((f == 0.0) && (mono_signbit (f) == 0)) {
				if (cfg->r4fp)
					amd64_sse_xorps_reg_reg (code, ins->dreg, ins->dreg);
				else
					amd64_sse_xorpd_reg_reg (code, ins->dreg, ins->dreg);
			} else {
				if (cfg->compile_aot && cfg->code_exec_only) {
					mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_R4_GOT, ins->inst_p0);
					amd64_mov_reg_membase (code, AMD64_R11, AMD64_RIP, 0, sizeof(gpointer));
					amd64_sse_movss_reg_membase (code, ins->dreg, AMD64_R11, 0);
				} else {
					mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_R4, ins->inst_p0);
					amd64_sse_movss_reg_membase (code, ins->dreg, AMD64_RIP, 0);
				}
				if (!cfg->r4fp)
					amd64_sse_cvtss2sd_reg_reg (code, ins->dreg, ins->dreg);
			}
			break;
		}
		case OP_STORER8_MEMBASE_REG:
			amd64_sse_movsd_membase_reg (code, ins->inst_destbasereg, ins->inst_offset, ins->sreg1);
			break;
		case OP_LOADR8_MEMBASE:
			amd64_sse_movsd_reg_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			break;
		case OP_STORER4_MEMBASE_REG:
			if (cfg->r4fp) {
				amd64_sse_movss_membase_reg (code, ins->inst_destbasereg, ins->inst_offset, ins->sreg1);
			} else {
				/* This requires a double->single conversion */
				amd64_sse_cvtsd2ss_reg_reg (code, MONO_ARCH_FP_SCRATCH_REG, ins->sreg1);
				amd64_sse_movss_membase_reg (code, ins->inst_destbasereg, ins->inst_offset, MONO_ARCH_FP_SCRATCH_REG);
			}
			break;
		case OP_LOADR4_MEMBASE:
			if (cfg->r4fp) {
				amd64_sse_movss_reg_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			} else {
				amd64_sse_movss_reg_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
				amd64_sse_cvtss2sd_reg_reg (code, ins->dreg, ins->dreg);
			}
			break;
		case OP_ICONV_TO_R4:
			if (cfg->r4fp) {
				amd64_sse_cvtsi2ss_reg_reg_size (code, ins->dreg, ins->sreg1, 4);
			} else {
				amd64_sse_cvtsi2ss_reg_reg_size (code, ins->dreg, ins->sreg1, 4);
				amd64_sse_cvtss2sd_reg_reg (code, ins->dreg, ins->dreg);
			}
			break;
		case OP_ICONV_TO_R8:
			amd64_sse_cvtsi2sd_reg_reg_size (code, ins->dreg, ins->sreg1, 4);
			break;
		case OP_LCONV_TO_R4:
			if (cfg->r4fp) {
				amd64_sse_cvtsi2ss_reg_reg (code, ins->dreg, ins->sreg1);
			} else {
				amd64_sse_cvtsi2ss_reg_reg (code, ins->dreg, ins->sreg1);
				amd64_sse_cvtss2sd_reg_reg (code, ins->dreg, ins->dreg);
			}
			break;
		case OP_LCONV_TO_R8:
			amd64_sse_cvtsi2sd_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_FCONV_TO_R4:
			if (cfg->r4fp) {
				amd64_sse_cvtsd2ss_reg_reg (code, ins->dreg, ins->sreg1);
			} else {
				amd64_sse_cvtsd2ss_reg_reg (code, ins->dreg, ins->sreg1);
				amd64_sse_cvtss2sd_reg_reg (code, ins->dreg, ins->dreg);
			}
			break;
		case OP_FCONV_TO_I1:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 1, TRUE);
			break;
		case OP_FCONV_TO_U1:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 1, FALSE);
			break;
		case OP_FCONV_TO_I2:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 2, TRUE);
			break;
		case OP_FCONV_TO_U2:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 2, FALSE);
			break;
		case OP_FCONV_TO_U4:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 4, FALSE);			
			break;
		case OP_FCONV_TO_I4:
		case OP_FCONV_TO_I:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 4, TRUE);
			break;
		case OP_FCONV_TO_I8:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 8, TRUE);
			break;

		case OP_RCONV_TO_I1:
			amd64_sse_cvtss2si_reg_reg_size (code, ins->dreg, ins->sreg1, 4);
			amd64_widen_reg (code, ins->dreg, ins->dreg, TRUE, FALSE);
			break;
		case OP_RCONV_TO_U1:
			amd64_sse_cvtss2si_reg_reg_size (code, ins->dreg, ins->sreg1, 4);
			amd64_widen_reg (code, ins->dreg, ins->dreg, FALSE, FALSE);
			break;
		case OP_RCONV_TO_I2:
			amd64_sse_cvtss2si_reg_reg_size (code, ins->dreg, ins->sreg1, 4);
			amd64_widen_reg (code, ins->dreg, ins->dreg, TRUE, TRUE);
			break;
		case OP_RCONV_TO_U2:
			amd64_sse_cvtss2si_reg_reg_size (code, ins->dreg, ins->sreg1, 4);
			amd64_widen_reg (code, ins->dreg, ins->dreg, FALSE, TRUE);
			break;
		case OP_RCONV_TO_I4:
			amd64_sse_cvtss2si_reg_reg_size (code, ins->dreg, ins->sreg1, 4);
			break;
		case OP_RCONV_TO_U4:
			// Use 8 as register size to get Nan/Inf conversion result truncated to 0
			amd64_sse_cvtss2si_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_RCONV_TO_I8:
		case OP_RCONV_TO_I:
			amd64_sse_cvtss2si_reg_reg_size (code, ins->dreg, ins->sreg1, 8);
			break;
		case OP_RCONV_TO_R8:
			amd64_sse_cvtss2sd_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_RCONV_TO_R4:
			if (ins->dreg != ins->sreg1)
				amd64_sse_movss_reg_reg (code, ins->dreg, ins->sreg1);
			break;

		case OP_LCONV_TO_R_UN: { 
			guint8 *br [2];

			/* Based on gcc code */
			amd64_test_reg_reg (code, ins->sreg1, ins->sreg1);
			br [0] = code; x86_branch8 (code, X86_CC_S, 0, TRUE);

			/* Positive case */
			amd64_sse_cvtsi2sd_reg_reg (code, ins->dreg, ins->sreg1);
			br [1] = code; x86_jump8 (code, 0);
			amd64_patch (br [0], code);

			/* Negative case */
			/* Save to the red zone */
			amd64_mov_membase_reg (code, AMD64_RSP, -8, AMD64_RAX, 8);
			amd64_mov_membase_reg (code, AMD64_RSP, -16, AMD64_RCX, 8);
			amd64_mov_reg_reg (code, AMD64_RCX, ins->sreg1, 8);
			amd64_mov_reg_reg (code, AMD64_RAX, ins->sreg1, 8);
			amd64_alu_reg_imm (code, X86_AND, AMD64_RCX, 1);
			amd64_shift_reg_imm (code, X86_SHR, AMD64_RAX, 1);
			amd64_alu_reg_imm (code, X86_OR, AMD64_RAX, AMD64_RCX);
			amd64_sse_cvtsi2sd_reg_reg (code, ins->dreg, AMD64_RAX);
			amd64_sse_addsd_reg_reg (code, ins->dreg, ins->dreg);
			/* Restore */
			amd64_mov_reg_membase (code, AMD64_RCX, AMD64_RSP, -16, 8);
			amd64_mov_reg_membase (code, AMD64_RAX, AMD64_RSP, -8, 8);
			amd64_patch (br [1], code);
			break;
		}
		case OP_LCONV_TO_OVF_U4:
			amd64_alu_reg_imm (code, X86_CMP, ins->sreg1, 0);
			EMIT_COND_SYSTEM_EXCEPTION (X86_CC_LT, TRUE, "OverflowException");
			amd64_mov_reg_reg (code, ins->dreg, ins->sreg1, 8);
			break;
		case OP_LCONV_TO_OVF_I4_UN:
			amd64_alu_reg_imm (code, X86_CMP, ins->sreg1, 0x7fffffff);
			EMIT_COND_SYSTEM_EXCEPTION (X86_CC_GT, FALSE, "OverflowException");
			amd64_mov_reg_reg (code, ins->dreg, ins->sreg1, 8);
			break;
		case OP_FMOVE:
			if (ins->dreg != ins->sreg1)
				amd64_sse_movsd_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_RMOVE:
			if (ins->dreg != ins->sreg1)
				amd64_sse_movss_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_MOVE_F_TO_I4:
			if (cfg->r4fp) {
				amd64_movd_reg_xreg_size (code, ins->dreg, ins->sreg1, 8);
			} else {
				amd64_sse_cvtsd2ss_reg_reg (code, MONO_ARCH_FP_SCRATCH_REG, ins->sreg1);
				amd64_movd_reg_xreg_size (code, ins->dreg, MONO_ARCH_FP_SCRATCH_REG, 8);
			}
			break;
		case OP_MOVE_I4_TO_F:
			amd64_movd_xreg_reg_size (code, ins->dreg, ins->sreg1, 8);
			if (!cfg->r4fp)
				amd64_sse_cvtss2sd_reg_reg (code, ins->dreg, ins->dreg);
			break;
		case OP_MOVE_F_TO_I8:
			amd64_movd_reg_xreg_size (code, ins->dreg, ins->sreg1, 8);
			break;
		case OP_MOVE_I8_TO_F:
			amd64_movd_xreg_reg_size (code, ins->dreg, ins->sreg1, 8);
			break;
		case OP_FADD:
			amd64_sse_addsd_reg_reg (code, ins->dreg, ins->sreg2);
			break;
		case OP_FSUB:
			amd64_sse_subsd_reg_reg (code, ins->dreg, ins->sreg2);
			break;		
		case OP_FMUL:
			amd64_sse_mulsd_reg_reg (code, ins->dreg, ins->sreg2);
			break;		
		case OP_FDIV:
			amd64_sse_divsd_reg_reg (code, ins->dreg, ins->sreg2);
			break;		
		case OP_FNEG: {
			static double r8_0 = -0.0;

			g_assert (ins->sreg1 == ins->dreg);

			if (cfg->compile_aot && cfg->code_exec_only) {
				mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_R8_GOT, &r8_0);
				amd64_mov_reg_membase (code, AMD64_R11, AMD64_RIP, 0, sizeof (target_mgreg_t));
				amd64_sse_movsd_reg_membase (code, MONO_ARCH_FP_SCRATCH_REG, AMD64_R11, 0);
				amd64_sse_xorpd_reg_reg (code, ins->dreg, MONO_ARCH_FP_SCRATCH_REG);
			} else {
				mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_R8, &r8_0);
				amd64_sse_xorpd_reg_membase (code, ins->dreg, AMD64_RIP, 0);
			}
			break;
		}
		case OP_ABS: {
			static guint64 d = 0x7fffffffffffffffUL;

			g_assert (ins->sreg1 == ins->dreg);

			if (cfg->compile_aot && cfg->code_exec_only) {
				mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_R8_GOT, &d);
				amd64_mov_reg_membase (code, AMD64_R11, AMD64_RIP, 0, sizeof (target_mgreg_t));
				amd64_sse_movsd_reg_membase (code, MONO_ARCH_FP_SCRATCH_REG, AMD64_R11, 0);
				amd64_sse_andpd_reg_reg (code, ins->dreg, MONO_ARCH_FP_SCRATCH_REG);
			} else {
				mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_R8, &d);
				amd64_sse_andpd_reg_membase (code, ins->dreg, AMD64_RIP, 0);
			}
			break;
		}
		case OP_SQRT:
			EMIT_SSE2_FPFUNC (code, fsqrt, ins->dreg, ins->sreg1);
			break;

		case OP_RADD:
			amd64_sse_addss_reg_reg (code, ins->dreg, ins->sreg2);
			break;
		case OP_RSUB:
			amd64_sse_subss_reg_reg (code, ins->dreg, ins->sreg2);
			break;
		case OP_RMUL:
			amd64_sse_mulss_reg_reg (code, ins->dreg, ins->sreg2);
			break;
		case OP_RDIV:
			amd64_sse_divss_reg_reg (code, ins->dreg, ins->sreg2);
			break;
		case OP_RNEG: {
			static float r4_0 = -0.0;

			g_assert (ins->sreg1 == ins->dreg);

			if (cfg->compile_aot && cfg->code_exec_only) {
				mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_R4_GOT, &r4_0);
				amd64_mov_reg_membase (code, AMD64_R11, AMD64_RIP, 0, sizeof (target_mgreg_t));
				amd64_sse_movss_reg_membase (code, MONO_ARCH_FP_SCRATCH_REG, AMD64_R11, 0);
			} else {
				mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_R4, &r4_0);
				amd64_sse_movss_reg_membase (code, MONO_ARCH_FP_SCRATCH_REG, AMD64_RIP, 0);
			}

			amd64_sse_xorps_reg_reg (code, ins->dreg, MONO_ARCH_FP_SCRATCH_REG);
			break;
		}

		case OP_IMIN:
			g_assert (cfg->opt & MONO_OPT_CMOV);
			g_assert (ins->dreg == ins->sreg1);
			amd64_alu_reg_reg_size (code, X86_CMP, ins->sreg1, ins->sreg2, 4);
			amd64_cmov_reg_size (code, X86_CC_GT, TRUE, ins->dreg, ins->sreg2, 4);
			break;
		case OP_IMIN_UN:
			g_assert (cfg->opt & MONO_OPT_CMOV);
			g_assert (ins->dreg == ins->sreg1);
			amd64_alu_reg_reg_size (code, X86_CMP, ins->sreg1, ins->sreg2, 4);
			amd64_cmov_reg_size (code, X86_CC_GT, FALSE, ins->dreg, ins->sreg2, 4);
			break;
		case OP_IMAX:
			g_assert (cfg->opt & MONO_OPT_CMOV);
			g_assert (ins->dreg == ins->sreg1);
			amd64_alu_reg_reg_size (code, X86_CMP, ins->sreg1, ins->sreg2, 4);
			amd64_cmov_reg_size (code, X86_CC_LT, TRUE, ins->dreg, ins->sreg2, 4);
			break;
		case OP_IMAX_UN:
			g_assert (cfg->opt & MONO_OPT_CMOV);
			g_assert (ins->dreg == ins->sreg1);
			amd64_alu_reg_reg_size (code, X86_CMP, ins->sreg1, ins->sreg2, 4);
			amd64_cmov_reg_size (code, X86_CC_LT, FALSE, ins->dreg, ins->sreg2, 4);
			break;
		case OP_LMIN:
			g_assert (cfg->opt & MONO_OPT_CMOV);
			g_assert (ins->dreg == ins->sreg1);
			amd64_alu_reg_reg (code, X86_CMP, ins->sreg1, ins->sreg2);
			amd64_cmov_reg (code, X86_CC_GT, TRUE, ins->dreg, ins->sreg2);
			break;
		case OP_LMIN_UN:
			g_assert (cfg->opt & MONO_OPT_CMOV);
			g_assert (ins->dreg == ins->sreg1);
			amd64_alu_reg_reg (code, X86_CMP, ins->sreg1, ins->sreg2);
			amd64_cmov_reg (code, X86_CC_GT, FALSE, ins->dreg, ins->sreg2);
			break;
		case OP_LMAX:
			g_assert (cfg->opt & MONO_OPT_CMOV);
			g_assert (ins->dreg == ins->sreg1);
			amd64_alu_reg_reg (code, X86_CMP, ins->sreg1, ins->sreg2);
			amd64_cmov_reg (code, X86_CC_LT, TRUE, ins->dreg, ins->sreg2);
			break;
		case OP_LMAX_UN:
			g_assert (cfg->opt & MONO_OPT_CMOV);
			g_assert (ins->dreg == ins->sreg1);
			amd64_alu_reg_reg (code, X86_CMP, ins->sreg1, ins->sreg2);
			amd64_cmov_reg (code, X86_CC_LT, FALSE, ins->dreg, ins->sreg2);
			break;	
		case OP_X86_FPOP:
			break;		
		case OP_FCOMPARE:
			/* 
			 * The two arguments are swapped because the fbranch instructions
			 * depend on this for the non-sse case to work.
			 */
			amd64_sse_comisd_reg_reg (code, ins->sreg2, ins->sreg1);
			break;
		case OP_RCOMPARE:
			/*
			 * FIXME: Get rid of this.
			 * The two arguments are swapped because the fbranch instructions
			 * depend on this for the non-sse case to work.
			 */
			amd64_sse_comiss_reg_reg (code, ins->sreg2, ins->sreg1);
			break;
		case OP_FCNEQ:
		case OP_FCEQ: {
			/* zeroing the register at the start results in 
			 * shorter and faster code (we can also remove the widening op)
			 */
			guchar *unordered_check;

			amd64_alu_reg_reg (code, X86_XOR, ins->dreg, ins->dreg);
			amd64_sse_comisd_reg_reg (code, ins->sreg1, ins->sreg2);
			unordered_check = code;
			x86_branch8 (code, X86_CC_P, 0, FALSE);

			if (ins->opcode == OP_FCEQ) {
				amd64_set_reg (code, X86_CC_EQ, ins->dreg, FALSE);
				amd64_patch (unordered_check, code);
			} else {
				guchar *jump_to_end;
				amd64_set_reg (code, X86_CC_NE, ins->dreg, FALSE);
				jump_to_end = code;
				x86_jump8 (code, 0);
				amd64_patch (unordered_check, code);
				amd64_inc_reg (code, ins->dreg);
				amd64_patch (jump_to_end, code);
			}
			break;
		}
		case OP_FCLT:
		case OP_FCLT_UN: {
			/* zeroing the register at the start results in 
			 * shorter and faster code (we can also remove the widening op)
			 */
			amd64_alu_reg_reg (code, X86_XOR, ins->dreg, ins->dreg);
			amd64_sse_comisd_reg_reg (code, ins->sreg2, ins->sreg1);
			if (ins->opcode == OP_FCLT_UN) {
				guchar *unordered_check = code;
				guchar *jump_to_end;
				x86_branch8 (code, X86_CC_P, 0, FALSE);
				amd64_set_reg (code, X86_CC_GT, ins->dreg, FALSE);
				jump_to_end = code;
				x86_jump8 (code, 0);
				amd64_patch (unordered_check, code);
				amd64_inc_reg (code, ins->dreg);
				amd64_patch (jump_to_end, code);
			} else {
				amd64_set_reg (code, X86_CC_GT, ins->dreg, FALSE);
			}
			break;
		}
		case OP_FCLE: {
			guchar *unordered_check;
			amd64_alu_reg_reg (code, X86_XOR, ins->dreg, ins->dreg);
			amd64_sse_comisd_reg_reg (code, ins->sreg2, ins->sreg1);
			unordered_check = code;
			x86_branch8 (code, X86_CC_P, 0, FALSE);
			amd64_set_reg (code, X86_CC_NB, ins->dreg, FALSE);
			amd64_patch (unordered_check, code);
			break;
		}
		case OP_FCGT:
		case OP_FCGT_UN: {
			/* zeroing the register at the start results in 
			 * shorter and faster code (we can also remove the widening op)
			 */
			guchar *unordered_check;

			amd64_alu_reg_reg (code, X86_XOR, ins->dreg, ins->dreg);
			amd64_sse_comisd_reg_reg (code, ins->sreg2, ins->sreg1);
			if (ins->opcode == OP_FCGT) {
				unordered_check = code;
				x86_branch8 (code, X86_CC_P, 0, FALSE);
				amd64_set_reg (code, X86_CC_LT, ins->dreg, FALSE);
				amd64_patch (unordered_check, code);
			} else {
				amd64_set_reg (code, X86_CC_LT, ins->dreg, FALSE);
			}
			break;
		}
		case OP_FCGE: {
			guchar *unordered_check;
			amd64_alu_reg_reg (code, X86_XOR, ins->dreg, ins->dreg);
			amd64_sse_comisd_reg_reg (code, ins->sreg2, ins->sreg1);
			unordered_check = code;
			x86_branch8 (code, X86_CC_P, 0, FALSE);
			amd64_set_reg (code, X86_CC_NA, ins->dreg, FALSE);
			amd64_patch (unordered_check, code);
			break;
		}

		case OP_RCEQ:
		case OP_RCGT:
		case OP_RCLT:
		case OP_RCLT_UN:
		case OP_RCGT_UN: {
			int x86_cond;

			amd64_alu_reg_reg (code, X86_XOR, ins->dreg, ins->dreg);
			amd64_sse_comiss_reg_reg (code, ins->sreg2, ins->sreg1);

			switch (ins->opcode) {
			case OP_RCEQ:
				x86_cond = X86_CC_EQ;
				break;
			case OP_RCGT:
				x86_cond = X86_CC_LT;
				break;
			case OP_RCLT:
				x86_cond = X86_CC_GT;
				break;
			case OP_RCLT_UN:
				x86_cond = X86_CC_GT;
				break;
			case OP_RCGT_UN:
				x86_cond = X86_CC_LT;
				break;
			default:
				g_assert_not_reached ();
				break;
			}

			guchar *unordered_check;

			switch (ins->opcode) {
			case OP_RCEQ:
			case OP_RCGT:
				unordered_check = code;
				x86_branch8 (code, X86_CC_P, 0, FALSE);
				amd64_set_reg (code, x86_cond, ins->dreg, FALSE);
				amd64_patch (unordered_check, code);
				break;
			case OP_RCLT_UN:
			case OP_RCGT_UN: {
				guchar *jump_to_end;

				unordered_check = code;
				x86_branch8 (code, X86_CC_P, 0, FALSE);
				amd64_set_reg (code, x86_cond, ins->dreg, FALSE);
				jump_to_end = code;
				x86_jump8 (code, 0);
				amd64_patch (unordered_check, code);
				amd64_inc_reg (code, ins->dreg);
				amd64_patch (jump_to_end, code);
				break;
			}
			case OP_RCLT:
				amd64_set_reg (code, x86_cond, ins->dreg, FALSE);
				break;
			default:
				g_assert_not_reached ();
				break;
			}
			break;
		}
		case OP_FCLT_MEMBASE:
		case OP_FCGT_MEMBASE:
		case OP_FCLT_UN_MEMBASE:
		case OP_FCGT_UN_MEMBASE:
		case OP_FCEQ_MEMBASE: {
			guchar *unordered_check, *jump_to_end;
			int x86_cond;

			amd64_alu_reg_reg (code, X86_XOR, ins->dreg, ins->dreg);
			amd64_sse_comisd_reg_membase (code, ins->sreg1, ins->sreg2, ins->inst_offset);

			switch (ins->opcode) {
			case OP_FCEQ_MEMBASE:
				x86_cond = X86_CC_EQ;
				break;
			case OP_FCLT_MEMBASE:
			case OP_FCLT_UN_MEMBASE:
				x86_cond = X86_CC_LT;
				break;
			case OP_FCGT_MEMBASE:
			case OP_FCGT_UN_MEMBASE:
				x86_cond = X86_CC_GT;
				break;
			default:
				g_assert_not_reached ();
			}

			unordered_check = code;
			x86_branch8 (code, X86_CC_P, 0, FALSE);
			amd64_set_reg (code, x86_cond, ins->dreg, FALSE);

			switch (ins->opcode) {
			case OP_FCEQ_MEMBASE:
			case OP_FCLT_MEMBASE:
			case OP_FCGT_MEMBASE:
				amd64_patch (unordered_check, code);
				break;
			case OP_FCLT_UN_MEMBASE:
			case OP_FCGT_UN_MEMBASE:
				jump_to_end = code;
				x86_jump8 (code, 0);
				amd64_patch (unordered_check, code);
				amd64_inc_reg (code, ins->dreg);
				amd64_patch (jump_to_end, code);
				break;
			default:
				break;
			}
			break;
		}
		case OP_FBEQ: {
			guchar *jump = code;
			x86_branch8 (code, X86_CC_P, 0, TRUE);
			EMIT_COND_BRANCH (ins, X86_CC_EQ, FALSE);
			amd64_patch (jump, code);
			break;
		}
		case OP_FBNE_UN:
			/* Branch if C013 != 100 */
			/* branch if !ZF or (PF|CF) */
			EMIT_COND_BRANCH (ins, X86_CC_NE, FALSE);
			EMIT_COND_BRANCH (ins, X86_CC_P, FALSE);
			EMIT_COND_BRANCH (ins, X86_CC_B, FALSE);
			break;
		case OP_FBLT:
			EMIT_COND_BRANCH (ins, X86_CC_GT, FALSE);
			break;
		case OP_FBLT_UN:
			EMIT_COND_BRANCH (ins, X86_CC_P, FALSE);
			EMIT_COND_BRANCH (ins, X86_CC_GT, FALSE);
			break;
		case OP_FBGT:
		case OP_FBGT_UN:
			if (ins->opcode == OP_FBGT) {
				guchar *br1;

				/* skip branch if C1=1 */
				br1 = code;
				x86_branch8 (code, X86_CC_P, 0, FALSE);
				/* branch if (C0 | C3) = 1 */
				EMIT_COND_BRANCH (ins, X86_CC_LT, FALSE);
				amd64_patch (br1, code);
				break;
			} else {
				EMIT_COND_BRANCH (ins, X86_CC_LT, FALSE);
			}
			break;
		case OP_FBGE: {
			/* Branch if C013 == 100 or 001 */
			guchar *br1;

			/* skip branch if C1=1 */
			br1 = code;
			x86_branch8 (code, X86_CC_P, 0, FALSE);
			/* branch if (C0 | C3) = 1 */
			EMIT_COND_BRANCH (ins, X86_CC_BE, FALSE);
			amd64_patch (br1, code);
			break;
		}
		case OP_FBGE_UN:
			/* Branch if C013 == 000 */
			EMIT_COND_BRANCH (ins, X86_CC_LE, FALSE);
			break;
		case OP_FBLE: {
			/* Branch if C013=000 or 100 */
			guchar *br1;

			/* skip branch if C1=1 */
			br1 = code;
			x86_branch8 (code, X86_CC_P, 0, FALSE);
			/* branch if C0=0 */
			EMIT_COND_BRANCH (ins, X86_CC_NB, FALSE);
			amd64_patch (br1, code);
			break;
		}
		case OP_FBLE_UN:
			/* Branch if C013 != 001 */
			EMIT_COND_BRANCH (ins, X86_CC_P, FALSE);
			EMIT_COND_BRANCH (ins, X86_CC_GE, FALSE);
			break;
		case OP_CKFINITE:
			/* Transfer value to the fp stack */
			amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, 16);
			amd64_movsd_membase_reg (code, AMD64_RSP, 0, ins->sreg1);
			amd64_fld_membase (code, AMD64_RSP, 0, TRUE);

			amd64_push_reg (code, AMD64_RAX);
			amd64_fxam (code);
			amd64_fnstsw (code);
			amd64_alu_reg_imm (code, X86_AND, AMD64_RAX, 0x4100);
			amd64_alu_reg_imm (code, X86_CMP, AMD64_RAX, X86_FP_C0);
			amd64_pop_reg (code, AMD64_RAX);
			amd64_fstp (code, 0);
			EMIT_COND_SYSTEM_EXCEPTION (X86_CC_EQ, FALSE, "OverflowException");
			amd64_alu_reg_imm (code, X86_ADD, AMD64_RSP, 16);
			break;
		case OP_TLS_GET: {
			code = mono_amd64_emit_tls_get (code, ins->dreg, ins->inst_offset);
			break;
		}
		case OP_TLS_SET: {
			code = mono_amd64_emit_tls_set (code, ins->sreg1, ins->inst_offset);
			break;
		}
		case OP_MEMORY_BARRIER: {
			if (ins->backend.memory_barrier_kind == MONO_MEMORY_BARRIER_SEQ)
				x86_mfence (code);
			break;
		}
		case OP_ATOMIC_ADD_I4:
		case OP_ATOMIC_ADD_I8: {
			int dreg = ins->dreg;
			guint32 size = (ins->opcode == OP_ATOMIC_ADD_I4) ? 4 : 8;

			if ((dreg == ins->sreg2) || (dreg == ins->inst_basereg))
				dreg = AMD64_R11;

			amd64_mov_reg_reg (code, dreg, ins->sreg2, size);
			amd64_prefix (code, X86_LOCK_PREFIX);
			amd64_xadd_membase_reg (code, ins->inst_basereg, ins->inst_offset, dreg, size);
			/* dreg contains the old value, add with sreg2 value */
			amd64_alu_reg_reg_size (code, X86_ADD, dreg, ins->sreg2, size);
			
			if (ins->dreg != dreg)
				amd64_mov_reg_reg (code, ins->dreg, dreg, size);

			break;
		}
		case OP_ATOMIC_EXCHANGE_I4:
		case OP_ATOMIC_EXCHANGE_I8: {
			guint32 size = ins->opcode == OP_ATOMIC_EXCHANGE_I4 ? 4 : 8;

			/* LOCK prefix is implied. */
			amd64_mov_reg_reg (code, GP_SCRATCH_REG, ins->sreg2, size);
			amd64_xchg_membase_reg_size (code, ins->sreg1, ins->inst_offset, GP_SCRATCH_REG, size);
			amd64_mov_reg_reg (code, ins->dreg, GP_SCRATCH_REG, size);
			break;
		}
		case OP_ATOMIC_CAS_I4:
		case OP_ATOMIC_CAS_I8: {
			guint32 size;

			if (ins->opcode == OP_ATOMIC_CAS_I8)
				size = 8;
			else
				size = 4;

			/* 
			 * See http://msdn.microsoft.com/en-us/magazine/cc302329.aspx for
			 * an explanation of how this works.
			 */
			g_assert (ins->sreg3 == AMD64_RAX);
			g_assert (ins->sreg1 != AMD64_RAX);
			g_assert (ins->sreg1 != ins->sreg2);

			amd64_prefix (code, X86_LOCK_PREFIX);
			amd64_cmpxchg_membase_reg_size (code, ins->sreg1, ins->inst_offset, ins->sreg2, size);

			if (ins->dreg != AMD64_RAX)
				amd64_mov_reg_reg (code, ins->dreg, AMD64_RAX, size);
			break;
		}
		case OP_ATOMIC_LOAD_I1: {
			amd64_widen_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset, TRUE, FALSE);
			break;
		}
		case OP_ATOMIC_LOAD_U1: {
			amd64_widen_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset, FALSE, FALSE);
			break;
		}
		case OP_ATOMIC_LOAD_I2: {
			amd64_widen_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset, TRUE, TRUE);
			break;
		}
		case OP_ATOMIC_LOAD_U2: {
			amd64_widen_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset, FALSE, TRUE);
			break;
		}
		case OP_ATOMIC_LOAD_I4: {
			amd64_movsxd_reg_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			break;
		}
		case OP_ATOMIC_LOAD_U4:
		case OP_ATOMIC_LOAD_I8:
		case OP_ATOMIC_LOAD_U8: {
			amd64_mov_reg_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset, ins->opcode == OP_ATOMIC_LOAD_U4 ? 4 : 8);
			break;
		}
		case OP_ATOMIC_LOAD_R4: {
			if (cfg->r4fp) {
				amd64_sse_movss_reg_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			} else {
				amd64_sse_movss_reg_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
				amd64_sse_cvtss2sd_reg_reg (code, ins->dreg, ins->dreg);
			}
			break;
		}
		case OP_ATOMIC_LOAD_R8: {
			amd64_sse_movsd_reg_membase (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			break;
		}
		case OP_ATOMIC_STORE_I1:
		case OP_ATOMIC_STORE_U1:
		case OP_ATOMIC_STORE_I2:
		case OP_ATOMIC_STORE_U2:
		case OP_ATOMIC_STORE_I4:
		case OP_ATOMIC_STORE_U4:
		case OP_ATOMIC_STORE_I8:
		case OP_ATOMIC_STORE_U8: {
			int size;

			switch (ins->opcode) {
			case OP_ATOMIC_STORE_I1:
			case OP_ATOMIC_STORE_U1:
				size = 1;
				break;
			case OP_ATOMIC_STORE_I2:
			case OP_ATOMIC_STORE_U2:
				size = 2;
				break;
			case OP_ATOMIC_STORE_I4:
			case OP_ATOMIC_STORE_U4:
				size = 4;
				break;
			case OP_ATOMIC_STORE_I8:
			case OP_ATOMIC_STORE_U8:
				size = 8;
				break;
			}

			amd64_mov_membase_reg (code, ins->inst_destbasereg, ins->inst_offset, ins->sreg1, size);

			if (ins->backend.memory_barrier_kind == MONO_MEMORY_BARRIER_SEQ)
				x86_mfence (code);
			break;
		}
		case OP_ATOMIC_STORE_R4: {
			if (cfg->r4fp) {
				amd64_sse_movss_membase_reg (code, ins->inst_destbasereg, ins->inst_offset, ins->sreg1);
			} else {
				amd64_sse_cvtsd2ss_reg_reg (code, MONO_ARCH_FP_SCRATCH_REG, ins->sreg1);
				amd64_sse_movss_membase_reg (code, ins->inst_destbasereg, ins->inst_offset, MONO_ARCH_FP_SCRATCH_REG);
			}

			if (ins->backend.memory_barrier_kind == MONO_MEMORY_BARRIER_SEQ)
				x86_mfence (code);
			break;
		}
		case OP_ATOMIC_STORE_R8: {
			x86_nop (code);
			x86_nop (code);
			amd64_sse_movsd_membase_reg (code, ins->inst_destbasereg, ins->inst_offset, ins->sreg1);
			x86_nop (code);
			x86_nop (code);

			if (ins->backend.memory_barrier_kind == MONO_MEMORY_BARRIER_SEQ)
				x86_mfence (code);
			break;
		}
		case OP_CARD_TABLE_WBARRIER: {
			int ptr = ins->sreg1;
			int value = ins->sreg2;
			guchar *br = 0;
			int nursery_shift, card_table_shift;
			gpointer card_table_mask;
			size_t nursery_size;

			gpointer card_table = mono_gc_get_card_table (&card_table_shift, &card_table_mask);
			guint64 nursery_start = (guint64)mono_gc_get_nursery (&nursery_shift, &nursery_size);
			guint64 shifted_nursery_start = nursery_start >> nursery_shift;

			/*If either point to the stack we can simply avoid the WB. This happens due to
			 * optimizations revealing a stack store that was not visible when op_cardtable was emited.
			 */
			if (ins->sreg1 == AMD64_RSP || ins->sreg2 == AMD64_RSP)
				continue;

			/*
			 * We need one register we can clobber, we choose EDX and make sreg1
			 * fixed EAX to work around limitations in the local register allocator.
			 * sreg2 might get allocated to EDX, but that is not a problem since
			 * we use it before clobbering EDX.
			 */
			g_assert (ins->sreg1 == AMD64_RAX);

			/*
			 * This is the code we produce:
			 *
			 *   edx = value
			 *   edx >>= nursery_shift
			 *   cmp edx, (nursery_start >> nursery_shift)
			 *   jne done
			 *   edx = ptr
			 *   edx >>= card_table_shift
			 *   edx += cardtable
			 *   [edx] = 1
			 * done:
			 */

			if (mono_gc_card_table_nursery_check ()) {
				if (value != AMD64_RDX)
					amd64_mov_reg_reg (code, AMD64_RDX, value, 8);
				amd64_shift_reg_imm (code, X86_SHR, AMD64_RDX, nursery_shift);
				if (shifted_nursery_start >> 31) {
					/*
					 * The value we need to compare against is 64 bits, so we need
					 * another spare register.  We use RBX, which we save and
					 * restore.
					 */
					amd64_mov_membase_reg (code, AMD64_RSP, -8, AMD64_RBX, 8);
					amd64_mov_reg_imm (code, AMD64_RBX, shifted_nursery_start);
					amd64_alu_reg_reg (code, X86_CMP, AMD64_RDX, AMD64_RBX);
					amd64_mov_reg_membase (code, AMD64_RBX, AMD64_RSP, -8, 8);
				} else {
					amd64_alu_reg_imm (code, X86_CMP, AMD64_RDX, shifted_nursery_start);
				}
				br = code; x86_branch8 (code, X86_CC_NE, -1, FALSE);
			}
			amd64_mov_reg_reg (code, AMD64_RDX, ptr, 8);
			amd64_shift_reg_imm (code, X86_SHR, AMD64_RDX, card_table_shift);
			if (card_table_mask)
				amd64_alu_reg_imm (code, X86_AND, AMD64_RDX, (guint32)(guint64)card_table_mask);

			mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_GC_CARD_TABLE_ADDR, card_table);
			amd64_alu_reg_membase (code, X86_ADD, AMD64_RDX, AMD64_RIP, 0);

			amd64_mov_membase_imm (code, AMD64_RDX, 0, 1, 1);

			if (mono_gc_card_table_nursery_check ())
				x86_patch (br, code);
			break;
		}
#ifdef MONO_ARCH_SIMD_INTRINSICS
		/* TODO: Some of these IR opcodes are marked as no clobber when they indeed do. */
		case OP_ADDPS:
			amd64_sse_addps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_DIVPS:
			amd64_sse_divps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_MULPS:
			amd64_sse_mulps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_SUBPS:
			amd64_sse_subps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_MAXPS:
			amd64_sse_maxps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_MINPS:
			amd64_sse_minps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_COMPPS:
			g_assert (ins->inst_c0 >= 0 && ins->inst_c0 <= 7);
			amd64_sse_cmpps_reg_reg_imm (code, ins->sreg1, ins->sreg2, ins->inst_c0);
			break;
		case OP_ANDPS:
			amd64_sse_andps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_ANDNPS:
			amd64_sse_andnps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_ORPS:
			amd64_sse_orps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_XORPS:
			amd64_sse_xorps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_SQRTPS:
			amd64_sse_sqrtps_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_RSQRTPS:
			amd64_sse_rsqrtps_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_RCPPS:
			amd64_sse_rcpps_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_ADDSUBPS:
			amd64_sse_addsubps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_HADDPS:
			amd64_sse_haddps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_HSUBPS:
			amd64_sse_hsubps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_DUPPS_HIGH:
			amd64_sse_movshdup_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_DUPPS_LOW:
			amd64_sse_movsldup_reg_reg (code, ins->dreg, ins->sreg1);
			break;

		case OP_PSHUFLEW_HIGH:
			g_assert (ins->inst_c0 >= 0 && ins->inst_c0 <= 0xFF);
			amd64_sse_pshufhw_reg_reg_imm (code, ins->dreg, ins->sreg1, ins->inst_c0);
			break;
		case OP_PSHUFLEW_LOW:
			g_assert (ins->inst_c0 >= 0 && ins->inst_c0 <= 0xFF);
			amd64_sse_pshuflw_reg_reg_imm (code, ins->dreg, ins->sreg1, ins->inst_c0);
			break;
		case OP_PSHUFLED:
			g_assert (ins->inst_c0 >= 0 && ins->inst_c0 <= 0xFF);
			amd64_sse_pshufd_reg_reg_imm (code, ins->dreg, ins->sreg1, ins->inst_c0);
			break;
		case OP_SHUFPS:
			g_assert (ins->inst_c0 >= 0 && ins->inst_c0 <= 0xFF);
			amd64_sse_shufps_reg_reg_imm (code, ins->sreg1, ins->sreg2, ins->inst_c0);
			break;
		case OP_SHUFPD:
			g_assert (ins->inst_c0 >= 0 && ins->inst_c0 <= 0x3);
			amd64_sse_shufpd_reg_reg_imm (code, ins->sreg1, ins->sreg2, ins->inst_c0);
			break;

		case OP_ADDPD:
			amd64_sse_addpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_DIVPD:
			amd64_sse_divpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_MULPD:
			amd64_sse_mulpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_SUBPD:
			amd64_sse_subpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_MAXPD:
			amd64_sse_maxpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_MINPD:
			amd64_sse_minpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_COMPPD:
			g_assert (ins->inst_c0 >= 0 && ins->inst_c0 <= 7);
			amd64_sse_cmppd_reg_reg_imm (code, ins->sreg1, ins->sreg2, ins->inst_c0);
			break;
		case OP_ANDPD:
			amd64_sse_andpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_ANDNPD:
			amd64_sse_andnpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_ORPD:
			amd64_sse_orpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_XORPD:
			amd64_sse_xorpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_SQRTPD:
			amd64_sse_sqrtpd_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_ADDSUBPD:
			amd64_sse_addsubpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_HADDPD:
			amd64_sse_haddpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_HSUBPD:
			amd64_sse_hsubpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_DUPPD:
			amd64_sse_movddup_reg_reg (code, ins->dreg, ins->sreg1);
			break;

		case OP_EXTRACT_MASK:
			amd64_sse_pmovmskb_reg_reg (code, ins->dreg, ins->sreg1);
			break;

		case OP_PAND:
			amd64_sse_pand_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PANDN:
			amd64_sse_pandn_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_POR:
			amd64_sse_por_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PXOR:
			amd64_sse_pxor_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PADDB:
			amd64_sse_paddb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PADDW:
			amd64_sse_paddw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PADDD:
			amd64_sse_paddd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PADDQ:
			amd64_sse_paddq_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PSUBB:
			amd64_sse_psubb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PSUBW:
			amd64_sse_psubw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PSUBD:
			amd64_sse_psubd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PSUBQ:
			amd64_sse_psubq_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PMAXB_UN:
			amd64_sse_pmaxub_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PMAXW_UN:
			amd64_sse_pmaxuw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PMAXD_UN:
			amd64_sse_pmaxud_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		
		case OP_PMAXB:
			amd64_sse_pmaxsb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PMAXW:
			amd64_sse_pmaxsw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PMAXD:
			amd64_sse_pmaxsd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PAVGB_UN:
			amd64_sse_pavgb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PAVGW_UN:
			amd64_sse_pavgw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PMINB_UN:
			amd64_sse_pminub_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PMINW_UN:
			amd64_sse_pminuw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PMIND_UN:
			amd64_sse_pminud_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PMINB:
			amd64_sse_pminsb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PMINW:
			amd64_sse_pminsw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PMIND:
			amd64_sse_pminsd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PCMPEQB:
			amd64_sse_pcmpeqb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PCMPEQW:
			amd64_sse_pcmpeqw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PCMPEQD:
			amd64_sse_pcmpeqd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PCMPEQQ:
			amd64_sse_pcmpeqq_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PCMPGTB:
			amd64_sse_pcmpgtb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PCMPGTW:
			amd64_sse_pcmpgtw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PCMPGTD:
			amd64_sse_pcmpgtd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PCMPGTQ:
			amd64_sse_pcmpgtq_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PSUM_ABS_DIFF:
			amd64_sse_psadbw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_UNPACK_LOWB:
			amd64_sse_punpcklbw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_UNPACK_LOWW:
			amd64_sse_punpcklwd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_UNPACK_LOWD:
			amd64_sse_punpckldq_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_UNPACK_LOWQ:
			amd64_sse_punpcklqdq_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_UNPACK_LOWPS:
			amd64_sse_unpcklps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_UNPACK_LOWPD:
			amd64_sse_unpcklpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_UNPACK_HIGHB:
			amd64_sse_punpckhbw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_UNPACK_HIGHW:
			amd64_sse_punpckhwd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_UNPACK_HIGHD:
			amd64_sse_punpckhdq_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_UNPACK_HIGHQ:
			amd64_sse_punpckhqdq_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_UNPACK_HIGHPS:
			amd64_sse_unpckhps_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_UNPACK_HIGHPD:
			amd64_sse_unpckhpd_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PACKW:
			amd64_sse_packsswb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PACKD:
			amd64_sse_packssdw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PACKW_UN:
			amd64_sse_packuswb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PACKD_UN:
			amd64_sse_packusdw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PADDB_SAT_UN:
			amd64_sse_paddusb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PSUBB_SAT_UN:
			amd64_sse_psubusb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PADDW_SAT_UN:
			amd64_sse_paddusw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PSUBW_SAT_UN:
			amd64_sse_psubusw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PADDB_SAT:
			amd64_sse_paddsb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PSUBB_SAT:
			amd64_sse_psubsb_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PADDW_SAT:
			amd64_sse_paddsw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PSUBW_SAT:
			amd64_sse_psubsw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
			
		case OP_PMULW:
			amd64_sse_pmullw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PMULD:
			amd64_sse_pmulld_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PMULQ:
			amd64_sse_pmuludq_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PMULW_HIGH_UN:
			amd64_sse_pmulhuw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;
		case OP_PMULW_HIGH:
			amd64_sse_pmulhw_reg_reg (code, ins->sreg1, ins->sreg2);
			break;

		case OP_PSHRW:
			amd64_sse_psrlw_reg_imm (code, ins->dreg, ins->inst_imm);
			break;
		case OP_PSHRW_REG:
			amd64_sse_psrlw_reg_reg (code, ins->dreg, ins->sreg2);
			break;

		case OP_PSARW:
			amd64_sse_psraw_reg_imm (code, ins->dreg, ins->inst_imm);
			break;
		case OP_PSARW_REG:
			amd64_sse_psraw_reg_reg (code, ins->dreg, ins->sreg2);
			break;

		case OP_PSHLW:
			amd64_sse_psllw_reg_imm (code, ins->dreg, ins->inst_imm);
			break;
		case OP_PSHLW_REG:
			amd64_sse_psllw_reg_reg (code, ins->dreg, ins->sreg2);
			break;

		case OP_PSHRD:
			amd64_sse_psrld_reg_imm (code, ins->dreg, ins->inst_imm);
			break;
		case OP_PSHRD_REG:
			amd64_sse_psrld_reg_reg (code, ins->dreg, ins->sreg2);
			break;

		case OP_PSARD:
			amd64_sse_psrad_reg_imm (code, ins->dreg, ins->inst_imm);
			break;
		case OP_PSARD_REG:
			amd64_sse_psrad_reg_reg (code, ins->dreg, ins->sreg2);
			break;

		case OP_PSHLD:
			amd64_sse_pslld_reg_imm (code, ins->dreg, ins->inst_imm);
			break;
		case OP_PSHLD_REG:
			amd64_sse_pslld_reg_reg (code, ins->dreg, ins->sreg2);
			break;

		case OP_PSHRQ:
			amd64_sse_psrlq_reg_imm (code, ins->dreg, ins->inst_imm);
			break;
		case OP_PSHRQ_REG:
			amd64_sse_psrlq_reg_reg (code, ins->dreg, ins->sreg2);
			break;
		
		/*TODO: This is appart of the sse spec but not added
		case OP_PSARQ:
			amd64_sse_psraq_reg_imm (code, ins->dreg, ins->inst_imm);
			break;
		case OP_PSARQ_REG:
			amd64_sse_psraq_reg_reg (code, ins->dreg, ins->sreg2);
			break;	
		*/
	
		case OP_PSHLQ:
			amd64_sse_psllq_reg_imm (code, ins->dreg, ins->inst_imm);
			break;
		case OP_PSHLQ_REG:
			amd64_sse_psllq_reg_reg (code, ins->dreg, ins->sreg2);
			break;	
		case OP_CVTDQ2PD:
			amd64_sse_cvtdq2pd_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_CVTDQ2PS:
			amd64_sse_cvtdq2ps_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_CVTPD2DQ:
			amd64_sse_cvtpd2dq_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_CVTPD2PS:
			amd64_sse_cvtpd2ps_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_CVTPS2DQ:
			amd64_sse_cvtps2dq_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_CVTPS2PD:
			amd64_sse_cvtps2pd_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_CVTTPD2DQ:
			amd64_sse_cvttpd2dq_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_CVTTPS2DQ:
			amd64_sse_cvttps2dq_reg_reg (code, ins->dreg, ins->sreg1);
			break;

		case OP_ICONV_TO_X:
			amd64_movd_xreg_reg_size (code, ins->dreg, ins->sreg1, 4);
			break;
		case OP_EXTRACT_I4:
			amd64_movd_reg_xreg_size (code, ins->dreg, ins->sreg1, 4);
			break;
		case OP_EXTRACT_I8:
			if (ins->inst_c0) {
				amd64_movhlps_reg_reg (code, MONO_ARCH_FP_SCRATCH_REG, ins->sreg1);
				amd64_movd_reg_xreg_size (code, ins->dreg, MONO_ARCH_FP_SCRATCH_REG, 8);
			} else {
				amd64_movd_reg_xreg_size (code, ins->dreg, ins->sreg1, 8);
			}
			break;
		case OP_EXTRACT_I1:
		case OP_EXTRACT_U1:
			amd64_movd_reg_xreg_size (code, ins->dreg, ins->sreg1, 4);
			if (ins->inst_c0)
				amd64_shift_reg_imm (code, X86_SHR, ins->dreg, ins->inst_c0 * 8);
			amd64_widen_reg (code, ins->dreg, ins->dreg, ins->opcode == OP_EXTRACT_I1, FALSE);
			break;
		case OP_EXTRACT_I2:
		case OP_EXTRACT_U2:
			/*amd64_movd_reg_xreg_size (code, ins->dreg, ins->sreg1, 4);
			if (ins->inst_c0)
				amd64_shift_reg_imm_size (code, X86_SHR, ins->dreg, 16, 4);*/
			amd64_sse_pextrw_reg_reg_imm (code, ins->dreg, ins->sreg1, ins->inst_c0);
			amd64_widen_reg_size (code, ins->dreg, ins->dreg, ins->opcode == OP_EXTRACT_I2, TRUE, 4);
			break;
		case OP_EXTRACT_R8:
			if (ins->inst_c0)
				amd64_movhlps_reg_reg (code, ins->dreg, ins->sreg1);
			else
				amd64_sse_movsd_reg_reg (code, ins->dreg, ins->sreg1);
			break;
		case OP_INSERT_I2:
			amd64_sse_pinsrw_reg_reg_imm (code, ins->sreg1, ins->sreg2, ins->inst_c0);
			break;
		case OP_EXTRACTX_U2:
			amd64_sse_pextrw_reg_reg_imm (code, ins->dreg, ins->sreg1, ins->inst_c0);
			break;
		case OP_INSERTX_U1_SLOW:
			/*sreg1 is the extracted ireg (scratch)
			/sreg2 is the to be inserted ireg (scratch)
			/dreg is the xreg to receive the value*/

			/*clear the bits from the extracted word*/
			amd64_alu_reg_imm (code, X86_AND, ins->sreg1, ins->inst_c0 & 1 ? 0x00FF : 0xFF00);
			/*shift the value to insert if needed*/
			if (ins->inst_c0 & 1)
				amd64_shift_reg_imm_size (code, X86_SHL, ins->sreg2, 8, 4);
			/*join them together*/
			amd64_alu_reg_reg (code, X86_OR, ins->sreg1, ins->sreg2);
			amd64_sse_pinsrw_reg_reg_imm (code, ins->dreg, ins->sreg1, ins->inst_c0 / 2);
			break;
		case OP_INSERTX_I4_SLOW:
			amd64_sse_pinsrw_reg_reg_imm (code, ins->dreg, ins->sreg2, ins->inst_c0 * 2);
			amd64_shift_reg_imm (code, X86_SHR, ins->sreg2, 16);
			amd64_sse_pinsrw_reg_reg_imm (code, ins->dreg, ins->sreg2, ins->inst_c0 * 2 + 1);
			break;
		case OP_INSERTX_I8_SLOW:
			amd64_movd_xreg_reg_size(code, MONO_ARCH_FP_SCRATCH_REG, ins->sreg2, 8);
			if (ins->inst_c0)
				amd64_movlhps_reg_reg (code, ins->dreg, MONO_ARCH_FP_SCRATCH_REG);
			else
				amd64_sse_movsd_reg_reg (code, ins->dreg, MONO_ARCH_FP_SCRATCH_REG);
			break;

		case OP_INSERTX_R4_SLOW:
			switch (ins->inst_c0) {
			case 0:
				if (cfg->r4fp)
					amd64_sse_movss_reg_reg (code, ins->dreg, ins->sreg2);
				else
					amd64_sse_cvtsd2ss_reg_reg (code, ins->dreg, ins->sreg2);
				break;
			case 1:
				amd64_sse_pshufd_reg_reg_imm (code, ins->dreg, ins->dreg, mono_simd_shuffle_mask(1, 0, 2, 3));
				if (cfg->r4fp)
					amd64_sse_movss_reg_reg (code, ins->dreg, ins->sreg2);
				else
					amd64_sse_cvtsd2ss_reg_reg (code, ins->dreg, ins->sreg2);
				amd64_sse_pshufd_reg_reg_imm (code, ins->dreg, ins->dreg, mono_simd_shuffle_mask(1, 0, 2, 3));
				break;
			case 2:
				amd64_sse_pshufd_reg_reg_imm (code, ins->dreg, ins->dreg, mono_simd_shuffle_mask(2, 1, 0, 3));
				if (cfg->r4fp)
					amd64_sse_movss_reg_reg (code, ins->dreg, ins->sreg2);
				else
					amd64_sse_cvtsd2ss_reg_reg (code, ins->dreg, ins->sreg2);
				amd64_sse_pshufd_reg_reg_imm (code, ins->dreg, ins->dreg, mono_simd_shuffle_mask(2, 1, 0, 3));
				break;
			case 3:
				amd64_sse_pshufd_reg_reg_imm (code, ins->dreg, ins->dreg, mono_simd_shuffle_mask(3, 1, 2, 0));
				if (cfg->r4fp)
					amd64_sse_movss_reg_reg (code, ins->dreg, ins->sreg2);
				else
					amd64_sse_cvtsd2ss_reg_reg (code, ins->dreg, ins->sreg2);
				amd64_sse_pshufd_reg_reg_imm (code, ins->dreg, ins->dreg, mono_simd_shuffle_mask(3, 1, 2, 0));
				break;
			}
			break;
		case OP_INSERTX_R8_SLOW:
			if (ins->inst_c0)
				amd64_movlhps_reg_reg (code, ins->dreg, ins->sreg2);
			else
				amd64_sse_movsd_reg_reg (code, ins->dreg, ins->sreg2);
			break;
		case OP_STOREX_MEMBASE_REG:
		case OP_STOREX_MEMBASE:
			amd64_sse_movups_membase_reg (code, ins->dreg, ins->inst_offset, ins->sreg1);
			break;
		case OP_LOADX_MEMBASE:
			amd64_sse_movups_reg_membase (code, ins->dreg, ins->sreg1, ins->inst_offset);
			break;
		case OP_LOADX_ALIGNED_MEMBASE:
			amd64_sse_movaps_reg_membase (code, ins->dreg, ins->sreg1, ins->inst_offset);
			break;
		case OP_STOREX_ALIGNED_MEMBASE_REG:
			amd64_sse_movaps_membase_reg (code, ins->dreg, ins->inst_offset, ins->sreg1);
			break;
		case OP_STOREX_NTA_MEMBASE_REG:
			amd64_sse_movntps_reg_membase (code, ins->dreg, ins->sreg1, ins->inst_offset);
			break;
		case OP_PREFETCH_MEMBASE:
			amd64_sse_prefetch_reg_membase (code, ins->backend.arg_info, ins->sreg1, ins->inst_offset);
			break;

		case OP_XMOVE:
			/*FIXME the peephole pass should have killed this*/
			if (ins->dreg != ins->sreg1)
				amd64_sse_movaps_reg_reg (code, ins->dreg, ins->sreg1);
			break;		
		case OP_XZERO:
			amd64_sse_pxor_reg_reg (code, ins->dreg, ins->dreg);
			break;
		case OP_XONES:
			amd64_sse_pcmpeqb_reg_reg (code, ins->dreg, ins->dreg);
			break;
		case OP_ICONV_TO_R4_RAW:
			amd64_movd_xreg_reg_size (code, ins->dreg, ins->sreg1, 4);
			if (!cfg->r4fp)
			  amd64_sse_cvtss2sd_reg_reg (code, ins->dreg, ins->dreg);
			break;

		case OP_FCONV_TO_R8_X:
			amd64_sse_movsd_reg_reg (code, ins->dreg, ins->sreg1);
			break;

		case OP_XCONV_R8_TO_I4:
			amd64_sse_cvttsd2si_reg_xreg_size (code, ins->dreg, ins->sreg1, 4);
			switch (ins->backend.source_opcode) {
			case OP_FCONV_TO_I1:
				amd64_widen_reg (code, ins->dreg, ins->dreg, TRUE, FALSE);
				break;
			case OP_FCONV_TO_U1:
				amd64_widen_reg (code, ins->dreg, ins->dreg, FALSE, FALSE);
				break;
			case OP_FCONV_TO_I2:
				amd64_widen_reg (code, ins->dreg, ins->dreg, TRUE, TRUE);
				break;
			case OP_FCONV_TO_U2:
				amd64_widen_reg (code, ins->dreg, ins->dreg, FALSE, TRUE);
				break;
			}			
			break;

		case OP_EXPAND_I2:
			amd64_sse_pinsrw_reg_reg_imm (code, ins->dreg, ins->sreg1, 0);
			amd64_sse_pinsrw_reg_reg_imm (code, ins->dreg, ins->sreg1, 1);
			amd64_sse_pshufd_reg_reg_imm (code, ins->dreg, ins->dreg, 0);
			break;
		case OP_EXPAND_I4:
			amd64_movd_xreg_reg_size (code, ins->dreg, ins->sreg1, 4);
			amd64_sse_pshufd_reg_reg_imm (code, ins->dreg, ins->dreg, 0);
			break;
		case OP_EXPAND_I8:
			amd64_movd_xreg_reg_size (code, ins->dreg, ins->sreg1, 8);
			amd64_sse_pshufd_reg_reg_imm (code, ins->dreg, ins->dreg, 0x44);
			break;
		case OP_EXPAND_R4:
			if (cfg->r4fp) {
				amd64_sse_movsd_reg_reg (code, ins->dreg, ins->sreg1);
			} else {
				amd64_sse_movsd_reg_reg (code, ins->dreg, ins->sreg1);
				amd64_sse_cvtsd2ss_reg_reg (code, ins->dreg, ins->dreg);
			}
			amd64_sse_pshufd_reg_reg_imm (code, ins->dreg, ins->dreg, 0);
			break;
		case OP_EXPAND_R8:
			amd64_sse_movsd_reg_reg (code, ins->dreg, ins->sreg1);
			amd64_sse_pshufd_reg_reg_imm (code, ins->dreg, ins->dreg, 0x44);
			break;
		case OP_SSE41_ROUNDP: {
			if (ins->inst_c1 == MONO_TYPE_R8)
				amd64_sse_roundpd_reg_reg_imm (code, ins->dreg, ins->sreg1, ins->inst_c0);
			else
				g_assert_not_reached (); // roundps, but it's not used anywhere for non-llvm back-end yet.
			break;
		}
#endif

		case OP_LZCNT32:
			amd64_sse_lzcnt_reg_reg_size (code, ins->dreg, ins->sreg1, 4);
			break;
		case OP_LZCNT64:
			amd64_sse_lzcnt_reg_reg_size (code, ins->dreg, ins->sreg1, 8);
			break;
		case OP_POPCNT32:
			amd64_sse_popcnt_reg_reg_size (code, ins->dreg, ins->sreg1, 4);
			break;
		case OP_POPCNT64:
			amd64_sse_popcnt_reg_reg_size (code, ins->dreg, ins->sreg1, 8);
			break;

		case OP_LIVERANGE_START: {
			if (cfg->verbose_level > 1)
				printf ("R%d START=0x%x\n", MONO_VARINFO (cfg, ins->inst_c0)->vreg, (int)(code - cfg->native_code));
			MONO_VARINFO (cfg, ins->inst_c0)->live_range_start = code - cfg->native_code;
			break;
		}
		case OP_LIVERANGE_END: {
			if (cfg->verbose_level > 1)
				printf ("R%d END=0x%x\n", MONO_VARINFO (cfg, ins->inst_c0)->vreg, (int)(code - cfg->native_code));
			MONO_VARINFO (cfg, ins->inst_c0)->live_range_end = code - cfg->native_code;
			break;
		}
		case OP_GC_SAFE_POINT: {
			guint8 *br [1];

			amd64_test_membase_imm_size (code, ins->sreg1, 0, 1, 4);
			br[0] = code; x86_branch8 (code, X86_CC_EQ, 0, FALSE);
			code = emit_call (cfg, NULL, code, MONO_JIT_ICALL_mono_threads_state_poll);
			amd64_patch (br[0], code);
			break;
		}

		case OP_GC_LIVENESS_DEF:
		case OP_GC_LIVENESS_USE:
		case OP_GC_PARAM_SLOT_LIVENESS_DEF:
			ins->backend.pc_offset = code - cfg->native_code;
			break;
		case OP_GC_SPILL_SLOT_LIVENESS_DEF:
			ins->backend.pc_offset = code - cfg->native_code;
			bb->spill_slot_defs = g_slist_prepend_mempool (cfg->mempool, bb->spill_slot_defs, ins);
			break;
		case OP_GET_LAST_ERROR:
			code = emit_get_last_error(code, ins->dreg);
			break;
		case OP_FILL_PROF_CALL_CTX:
			for (int i = 0; i < AMD64_NREG; i++)
				if (AMD64_IS_CALLEE_SAVED_REG (i) || i == AMD64_RSP)
					amd64_mov_membase_reg (code, ins->sreg1, MONO_STRUCT_OFFSET (MonoContext, gregs) + i * sizeof (target_mgreg_t), i, sizeof (target_mgreg_t));
			break;
		default:
			g_warning ("unknown opcode %s in %s()\n", mono_inst_name (ins->opcode), __FUNCTION__);
			g_assert_not_reached ();
		}

		g_assertf ((code - cfg->native_code - offset) <= max_len,
			   "wrong maximal instruction length of instruction %s (expected %d, got %d)",
			   mono_inst_name (ins->opcode), max_len, (int)(code - cfg->native_code - offset));
	}

	set_code_cursor (cfg, code);
}

#endif /* DISABLE_JIT */

#ifndef DISABLE_JIT

static int
get_max_epilog_size (MonoCompile *cfg)
{
	int max_epilog_size = 16;

	if (cfg->method->save_lmf)
		max_epilog_size += 256;

	max_epilog_size += (AMD64_NREG * 2);

	return max_epilog_size;
}

/*
 * This macro is used for testing whenever the unwinder works correctly at every point
 * where an async exception can happen.
 */
/* This will generate a SIGSEGV at the given point in the code */
#define async_exc_point(code) do { \
    if (mono_inject_async_exc_method && mono_method_desc_full_match (mono_inject_async_exc_method, cfg->method)) { \
         if (cfg->arch.async_point_count == mono_inject_async_exc_pos) \
             amd64_mov_reg_mem (code, AMD64_RAX, 0, 4); \
         cfg->arch.async_point_count ++; \
    } \
} while (0)

#ifdef TARGET_WIN32
static guint8 *
emit_prolog_setup_sp_win64 (MonoCompile *cfg, guint8 *code, int alloc_size, int *cfa_offset_input)
{
	int cfa_offset = *cfa_offset_input;

	/* Allocate windows stack frame using stack probing method */
	if (alloc_size) {

		if (alloc_size >= 0x1000) {
			amd64_mov_reg_imm (code, AMD64_RAX, alloc_size);
			code = emit_call (cfg, NULL, code, MONO_JIT_ICALL_mono_chkstk_win64);
		}

		amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, alloc_size);
		if (cfg->arch.omit_fp) {
			cfa_offset += alloc_size;
			mono_emit_unwind_op_def_cfa_offset (cfg, code, cfa_offset);
			async_exc_point (code);
		}

		// NOTE, in a standard win64 prolog the alloc unwind info is always emitted, but since mono
		// uses a frame pointer with negative offsets and a standard win64 prolog assumes positive offsets, we can't
		// emit sp alloc unwind metadata since the native OS unwinder will incorrectly restore sp. Excluding the alloc
		// metadata on the other hand won't give the OS the information so it can just restore the frame pointer to sp and
		// that will retrieve the expected results.
		if (cfg->arch.omit_fp)
			mono_emit_unwind_op_sp_alloc (cfg, code, alloc_size);
	}

	*cfa_offset_input = cfa_offset;
	set_code_cursor (cfg, code);
	return code;
}
#endif /* TARGET_WIN32 */

guint8 *
mono_arch_emit_prolog (MonoCompile *cfg)
{
	MonoMethod *method = cfg->method;
	MonoBasicBlock *bb;
	MonoMethodSignature *sig;
	MonoInst *ins;
	int alloc_size, pos, i, cfa_offset, quad, max_epilog_size, save_area_offset;
	guint8 *code;
	CallInfo *cinfo;
	MonoInst *lmf_var = cfg->lmf_var;
	gboolean args_clobbered = FALSE;

	cfg->code_size = MAX (cfg->header->code_size * 4, 1024);

	code = cfg->native_code = (unsigned char *)g_malloc (cfg->code_size);

	/* Amount of stack space allocated by register saving code */
	pos = 0;

	/* Offset between RSP and the CFA */
	cfa_offset = 0;

	/* 
	 * The prolog consists of the following parts:
	 * FP present:
	 * - push rbp
	 * - mov rbp, rsp
	 * - save callee saved regs using moves
	 * - allocate frame
	 * - save rgctx if needed
	 * - save lmf if needed
	 * FP not present:
	 * - allocate frame
	 * - save rgctx if needed
	 * - save lmf if needed
	 * - save callee saved regs using moves
	 */

	// CFA = sp + 8
	cfa_offset = 8;
	mono_emit_unwind_op_def_cfa (cfg, code, AMD64_RSP, 8);
	// IP saved at CFA - 8
	mono_emit_unwind_op_offset (cfg, code, AMD64_RIP, -cfa_offset);
	async_exc_point (code);
	mini_gc_set_slot_type_from_cfa (cfg, -cfa_offset, SLOT_NOREF);

	if (!cfg->arch.omit_fp) {
		amd64_push_reg (code, AMD64_RBP);
		cfa_offset += 8;
		mono_emit_unwind_op_def_cfa_offset (cfg, code, cfa_offset);
		mono_emit_unwind_op_offset (cfg, code, AMD64_RBP, - cfa_offset);
		async_exc_point (code);
		/* These are handled automatically by the stack marking code */
		mini_gc_set_slot_type_from_cfa (cfg, -cfa_offset, SLOT_NOREF);

		amd64_mov_reg_reg (code, AMD64_RBP, AMD64_RSP, sizeof (target_mgreg_t));
		mono_emit_unwind_op_def_cfa_reg (cfg, code, AMD64_RBP);
		mono_emit_unwind_op_fp_alloc (cfg, code, AMD64_RBP, 0);
		async_exc_point (code);
	}

	/* The param area is always at offset 0 from sp */
	/* This needs to be allocated here, since it has to come after the spill area */
	if (cfg->param_area) {
		if (cfg->arch.omit_fp)
			// FIXME:
			g_assert_not_reached ();
		cfg->stack_offset += ALIGN_TO (cfg->param_area, sizeof (target_mgreg_t));
	}

	if (cfg->arch.omit_fp) {
		/* 
		 * On enter, the stack is misaligned by the pushing of the return
		 * address. It is either made aligned by the pushing of %rbp, or by
		 * this.
		 */
		alloc_size = ALIGN_TO (cfg->stack_offset, 8);
		if ((alloc_size % 16) == 0) {
			alloc_size += 8;
			/* Mark the padding slot as NOREF */
			mini_gc_set_slot_type_from_cfa (cfg, -cfa_offset - sizeof (target_mgreg_t), SLOT_NOREF);
		}
	} else {
		alloc_size = ALIGN_TO (cfg->stack_offset, MONO_ARCH_FRAME_ALIGNMENT);
		if (cfg->stack_offset != alloc_size) {
			/* Mark the padding slot as NOREF */
			mini_gc_set_slot_type_from_fp (cfg, -alloc_size + cfg->param_area, SLOT_NOREF);
		}
		cfg->arch.sp_fp_offset = alloc_size;
		alloc_size -= pos;
	}

	cfg->arch.stack_alloc_size = alloc_size;

	set_code_cursor (cfg, code);

	/* Allocate stack frame */
#ifdef TARGET_WIN32
	code = emit_prolog_setup_sp_win64 (cfg, code, alloc_size, &cfa_offset);
#else
	if (alloc_size) {
		/* See mono_emit_stack_alloc */
#if defined(MONO_ARCH_SIGSEGV_ON_ALTSTACK)
		guint32 remaining_size = alloc_size;

		/* Use a loop for large sizes */
		if (remaining_size > 10 * 0x1000) {
			amd64_mov_reg_imm (code, X86_EAX, remaining_size / 0x1000);
			guint8 *label = code;
			amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, 0x1000);
			amd64_test_membase_reg (code, AMD64_RSP, 0, AMD64_RSP);
			amd64_alu_reg_imm (code, X86_SUB, AMD64_RAX, 1);
			amd64_alu_reg_imm (code, X86_CMP, AMD64_RAX, 0);
			guint8 *label2 = code;
			x86_branch8 (code, X86_CC_NE, 0, FALSE);
			amd64_patch (label2, label);
			if (cfg->arch.omit_fp) {
				cfa_offset += (remaining_size / 0x1000) * 0x1000;
				mono_emit_unwind_op_def_cfa_offset (cfg, code, cfa_offset);
			}

			remaining_size = remaining_size % 0x1000;
			set_code_cursor (cfg, code);
		}

		guint32 required_code_size = ((remaining_size / 0x1000) + 1) * 11; /*11 is the max size of amd64_alu_reg_imm + amd64_test_membase_reg*/
		code = realloc_code (cfg, required_code_size);

		while (remaining_size >= 0x1000) {
			amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, 0x1000);
 			if (cfg->arch.omit_fp) {
				cfa_offset += 0x1000;
 				mono_emit_unwind_op_def_cfa_offset (cfg, code, cfa_offset);
			}
			async_exc_point (code);

			amd64_test_membase_reg (code, AMD64_RSP, 0, AMD64_RSP);
			remaining_size -= 0x1000;
		}
		if (remaining_size) {
			amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, remaining_size);
 			if (cfg->arch.omit_fp) {
				cfa_offset += remaining_size;
 				mono_emit_unwind_op_def_cfa_offset (cfg, code, cfa_offset);
				async_exc_point (code);
			}
		}
#else
		amd64_alu_reg_imm (code, X86_SUB, AMD64_RSP, alloc_size);
		if (cfg->arch.omit_fp) {
			cfa_offset += alloc_size;
			mono_emit_unwind_op_def_cfa_offset (cfg, code, cfa_offset);
			async_exc_point (code);
		}
#endif
	}
#endif

	/* Stack alignment check */
#if 0
	{
		guint8 *buf;

		amd64_mov_reg_reg (code, AMD64_RAX, AMD64_RSP, 8);
		amd64_alu_reg_imm (code, X86_AND, AMD64_RAX, 0xf);
		amd64_alu_reg_imm (code, X86_CMP, AMD64_RAX, 0);
		buf = code;
		x86_branch8 (code, X86_CC_EQ, 1, FALSE);
		amd64_breakpoint (code);
		amd64_patch (buf, code);
	}
#endif

	if (mini_debug_options.init_stacks) {
		/* Fill the stack frame with a dummy value to force deterministic behavior */
	
		/* Save registers to the red zone */
		amd64_mov_membase_reg (code, AMD64_RSP, -8, AMD64_RDI, 8);
		amd64_mov_membase_reg (code, AMD64_RSP, -16, AMD64_RCX, 8);

MONO_DISABLE_WARNING (4310) // cast truncates constant value
		amd64_mov_reg_imm (code, AMD64_RAX, 0x2a2a2a2a2a2a2a2a);
MONO_RESTORE_WARNING

		amd64_mov_reg_imm (code, AMD64_RCX, alloc_size / 8);
		amd64_mov_reg_reg (code, AMD64_RDI, AMD64_RSP, 8);

		amd64_cld (code);
		amd64_prefix (code, X86_REP_PREFIX);
		amd64_stosl (code);

		amd64_mov_reg_membase (code, AMD64_RDI, AMD64_RSP, -8, 8);
		amd64_mov_reg_membase (code, AMD64_RCX, AMD64_RSP, -16, 8);
	}

	/* Save LMF */
	if (method->save_lmf)
		code = emit_setup_lmf (cfg, code, lmf_var->inst_offset, cfa_offset);

	/* Save callee saved registers */
	if (cfg->arch.omit_fp) {
		save_area_offset = cfg->arch.reg_save_area_offset;
		/* Save caller saved registers after sp is adjusted */
		/* The registers are saved at the bottom of the frame */
		/* FIXME: Optimize this so the regs are saved at the end of the frame in increasing order */
	} else {
		/* The registers are saved just below the saved rbp */
		save_area_offset = cfg->arch.reg_save_area_offset;
	}

	for (i = 0; i < AMD64_NREG; ++i) {
		if (AMD64_IS_CALLEE_SAVED_REG (i) && (cfg->arch.saved_iregs & (1 << i))) {
			amd64_mov_membase_reg (code, cfg->frame_reg, save_area_offset, i, 8);

			if (cfg->arch.omit_fp) {
				mono_emit_unwind_op_offset (cfg, code, i, - (cfa_offset - save_area_offset));
				/* These are handled automatically by the stack marking code */
				mini_gc_set_slot_type_from_cfa (cfg, - (cfa_offset - save_area_offset), SLOT_NOREF);
			} else {
				mono_emit_unwind_op_offset (cfg, code, i, - (-save_area_offset + (2 * 8)));
				// FIXME: GC
			}

			save_area_offset += 8;
			async_exc_point (code);
		}
	}

	/* store runtime generic context */
	if (cfg->rgctx_var) {
		g_assert (cfg->rgctx_var->opcode == OP_REGOFFSET &&
				(cfg->rgctx_var->inst_basereg == AMD64_RBP || cfg->rgctx_var->inst_basereg == AMD64_RSP));

		amd64_mov_membase_reg (code, cfg->rgctx_var->inst_basereg, cfg->rgctx_var->inst_offset, MONO_ARCH_RGCTX_REG, sizeof(gpointer));

		mono_add_var_location (cfg, cfg->rgctx_var, TRUE, MONO_ARCH_RGCTX_REG, 0, 0, code - cfg->native_code);
		mono_add_var_location (cfg, cfg->rgctx_var, FALSE, cfg->rgctx_var->inst_basereg, cfg->rgctx_var->inst_offset, code - cfg->native_code, 0);
	}

	/* compute max_length in order to use short forward jumps */
	max_epilog_size = get_max_epilog_size (cfg);
	if (cfg->opt & MONO_OPT_BRANCH && cfg->max_block_num < MAX_BBLOCKS_FOR_BRANCH_OPTS) {
		for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
			MonoInst *ins;
			int max_length = 0;

			/* max alignment for loops */
			if ((cfg->opt & MONO_OPT_LOOP) && bb_is_loop_start (bb))
				max_length += LOOP_ALIGNMENT;

			MONO_BB_FOR_EACH_INS (bb, ins) {
				max_length += ins_get_size (ins->opcode);
			}

			/* Take prolog and epilog instrumentation into account */
			if (bb == cfg->bb_entry || bb == cfg->bb_exit)
				max_length += max_epilog_size;
			
			bb->max_length = max_length;
		}
	}

	sig = mono_method_signature_internal (method);
	pos = 0;

	cinfo = cfg->arch.cinfo;

	if (sig->ret->type != MONO_TYPE_VOID) {
		/* Save volatile arguments to the stack */
		if (cfg->vret_addr && (cfg->vret_addr->opcode != OP_REGVAR))
			amd64_mov_membase_reg (code, cfg->vret_addr->inst_basereg, cfg->vret_addr->inst_offset, cinfo->ret.reg, 8);
	}

	/* Keep this in sync with emit_load_volatile_arguments */
	for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
		ArgInfo *ainfo = cinfo->args + i;

		ins = cfg->args [i];

		if (ins->flags & MONO_INST_IS_DEAD && !MONO_CFG_PROFILE (cfg, ENTER_CONTEXT))
			/* Unused arguments */
			continue;

		/* Save volatile arguments to the stack */
		if (ins->opcode != OP_REGVAR) {
			switch (ainfo->storage) {
			case ArgInIReg: {
				guint32 size = 8;

				/* FIXME: I1 etc */
				/*
				if (stack_offset & 0x1)
					size = 1;
				else if (stack_offset & 0x2)
					size = 2;
				else if (stack_offset & 0x4)
					size = 4;
				else
					size = 8;
				*/
				amd64_mov_membase_reg (code, ins->inst_basereg, ins->inst_offset, ainfo->reg, size);

				/*
				 * Save the original location of 'this',
				 * get_generic_info_from_stack_frame () needs this to properly look up
				 * the argument value during the handling of async exceptions.
				 */
				if (i == 0 && sig->hasthis) {
					mono_add_var_location (cfg, ins, TRUE, ainfo->reg, 0, 0, code - cfg->native_code);
					mono_add_var_location (cfg, ins, FALSE, ins->inst_basereg, ins->inst_offset, code - cfg->native_code, 0);
				}
				break;
			}
			case ArgInFloatSSEReg:
				amd64_movss_membase_reg (code, ins->inst_basereg, ins->inst_offset, ainfo->reg);
				break;
			case ArgInDoubleSSEReg:
				amd64_movsd_membase_reg (code, ins->inst_basereg, ins->inst_offset, ainfo->reg);
				break;
			case ArgValuetypeInReg:
				for (quad = 0; quad < 2; quad ++) {
					switch (ainfo->pair_storage [quad]) {
					case ArgInIReg:
						amd64_mov_membase_reg (code, ins->inst_basereg, ins->inst_offset + (quad * sizeof (target_mgreg_t)), ainfo->pair_regs [quad], sizeof (target_mgreg_t));
						break;
					case ArgInFloatSSEReg:
						amd64_movss_membase_reg (code, ins->inst_basereg, ins->inst_offset + (quad * sizeof (target_mgreg_t)), ainfo->pair_regs [quad]);
						break;
					case ArgInDoubleSSEReg:
						amd64_movsd_membase_reg (code, ins->inst_basereg, ins->inst_offset + (quad * sizeof (target_mgreg_t)), ainfo->pair_regs [quad]);
						break;
					case ArgNone:
						break;
					default:
						g_assert_not_reached ();
					}
				}
				break;
			case ArgValuetypeAddrInIReg:
				if (ainfo->pair_storage [0] == ArgInIReg)
					amd64_mov_membase_reg (code, ins->inst_left->inst_basereg, ins->inst_left->inst_offset, ainfo->pair_regs [0], sizeof (target_mgreg_t));
				break;
			case ArgValuetypeAddrOnStack:
				break;
			case ArgGSharedVtInReg:
				amd64_mov_membase_reg (code, ins->inst_basereg, ins->inst_offset, ainfo->reg, 8);
				break;
			default:
				break;
			}
		} else {
			/* Argument allocated to (non-volatile) register */
			switch (ainfo->storage) {
			case ArgInIReg:
				amd64_mov_reg_reg (code, ins->dreg, ainfo->reg, 8);
				break;
			case ArgOnStack:
				amd64_mov_reg_membase (code, ins->dreg, AMD64_RBP, ARGS_OFFSET + ainfo->offset, 8);
				break;
			default:
				g_assert_not_reached ();
			}

			if (i == 0 && sig->hasthis) {
				g_assert (ainfo->storage == ArgInIReg);
				mono_add_var_location (cfg, ins, TRUE, ainfo->reg, 0, 0, code - cfg->native_code);
				mono_add_var_location (cfg, ins, TRUE, ins->dreg, 0, code - cfg->native_code, 0);
			}
		}
	}

	if (cfg->method->save_lmf)
		args_clobbered = TRUE;

	/*
	 * Optimize the common case of the first bblock making a call with the same
	 * arguments as the method. This works because the arguments are still in their
	 * original argument registers.
	 * FIXME: Generalize this
	 */
	if (!args_clobbered) {
		MonoBasicBlock *first_bb = cfg->bb_entry;
		MonoInst *next;
		int filter = FILTER_IL_SEQ_POINT;

		next = mono_bb_first_inst (first_bb, filter);
		if (!next && first_bb->next_bb) {
			first_bb = first_bb->next_bb;
			next = mono_bb_first_inst (first_bb, filter);
		}

		if (first_bb->in_count > 1)
			next = NULL;

		for (i = 0; next && i < sig->param_count + sig->hasthis; ++i) {
			ArgInfo *ainfo = cinfo->args + i;
			gboolean match = FALSE;

			ins = cfg->args [i];
			if (ins->opcode != OP_REGVAR) {
				switch (ainfo->storage) {
				case ArgInIReg: {
					if (((next->opcode == OP_LOAD_MEMBASE) || (next->opcode == OP_LOADI4_MEMBASE)) && next->inst_basereg == ins->inst_basereg && next->inst_offset == ins->inst_offset) {
						if (next->dreg == ainfo->reg) {
							NULLIFY_INS (next);
							match = TRUE;
						} else {
							next->opcode = OP_MOVE;
							next->sreg1 = ainfo->reg;
							/* Only continue if the instruction doesn't change argument regs */
							if (next->dreg == ainfo->reg || next->dreg == AMD64_RAX)
								match = TRUE;
						}
					}
					break;
				}
				default:
					break;
				}
			} else {
				/* Argument allocated to (non-volatile) register */
				switch (ainfo->storage) {
				case ArgInIReg:
					if (next->opcode == OP_MOVE && next->sreg1 == ins->dreg && next->dreg == ainfo->reg) {
						NULLIFY_INS (next);
						match = TRUE;
					}
					break;
				default:
					break;
				}
			}

			if (match) {
				next = mono_inst_next (next, filter);
				//next = mono_inst_list_next (&next->node, &first_bb->ins_list);
				if (!next)
					break;
			}
		}
	}

	if (cfg->gen_sdb_seq_points) {
		MonoInst *info_var = cfg->arch.seq_point_info_var;

		/* Initialize seq_point_info_var */
		if (cfg->compile_aot) {
			/* Initialize the variable from a GOT slot */
			/* Same as OP_AOTCONST */
			mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_SEQ_POINT_INFO, cfg->method);
			amd64_mov_reg_membase (code, AMD64_R11, AMD64_RIP, 0, sizeof(gpointer));
			g_assert (info_var->opcode == OP_REGOFFSET);
			amd64_mov_membase_reg (code, info_var->inst_basereg, info_var->inst_offset, AMD64_R11, 8);
		}

		if (cfg->compile_aot) {
			/* Initialize ss_tramp_var */
			ins = cfg->arch.ss_tramp_var;
			g_assert (ins->opcode == OP_REGOFFSET);

			amd64_mov_reg_membase (code, AMD64_R11, info_var->inst_basereg, info_var->inst_offset, 8);
			amd64_mov_reg_membase (code, AMD64_R11, AMD64_R11, MONO_STRUCT_OFFSET (SeqPointInfo, ss_tramp_addr), 8);
			amd64_mov_membase_reg (code, ins->inst_basereg, ins->inst_offset, AMD64_R11, 8);
		} else {
			/* Initialize ss_tramp_var */
			ins = cfg->arch.ss_tramp_var;
			g_assert (ins->opcode == OP_REGOFFSET);

			amd64_mov_reg_imm (code, AMD64_R11, (guint64)&mono_amd64_ss_trampoline);
			amd64_mov_membase_reg (code, ins->inst_basereg, ins->inst_offset, AMD64_R11, 8);

			/* Initialize bp_tramp_var */
			ins = cfg->arch.bp_tramp_var;
			g_assert (ins->opcode == OP_REGOFFSET);

			amd64_mov_reg_imm (code, AMD64_R11, (guint64)&mono_amd64_bp_trampoline);
			amd64_mov_membase_reg (code, ins->inst_basereg, ins->inst_offset, AMD64_R11, 8);
		}
	}

	set_code_cursor (cfg, code);

	return code;
}

void
mono_arch_emit_epilog (MonoCompile *cfg)
{
	MonoMethod *method = cfg->method;
	int quad, i;
	guint8 *code;
	int max_epilog_size;
	CallInfo *cinfo;
	gint32 lmf_offset = cfg->lmf_var ? cfg->lmf_var->inst_offset : -1;
	gint32 save_area_offset = cfg->arch.reg_save_area_offset;

	max_epilog_size = get_max_epilog_size (cfg);

	code = realloc_code (cfg, max_epilog_size);

	cfg->has_unwind_info_for_epilog = TRUE;

	/* Mark the start of the epilog */
	mono_emit_unwind_op_mark_loc (cfg, code, 0);

	/* Save the uwind state which is needed by the out-of-line code */
	mono_emit_unwind_op_remember_state (cfg, code);

	/* the code restoring the registers must be kept in sync with OP_TAILCALL */
	
	if (method->save_lmf) {
		if (cfg->used_int_regs & (1 << AMD64_RBP))
			amd64_mov_reg_membase (code, AMD64_RBP, cfg->frame_reg, lmf_offset + MONO_STRUCT_OFFSET (MonoLMF, rbp), 8);
		if (cfg->arch.omit_fp)
			/*
			 * emit_setup_lmf () marks RBP as saved, we have to mark it as same value here before clearing up the stack
			 * since its stack slot will become invalid.
			 */
			mono_emit_unwind_op_same_value (cfg, code, AMD64_RBP);
	}

	/* Restore callee saved regs */
	for (i = 0; i < AMD64_NREG; ++i) {
		if (AMD64_IS_CALLEE_SAVED_REG (i) && (cfg->arch.saved_iregs & (1 << i))) {
			/* Restore only used_int_regs, not arch.saved_iregs */
#if defined(MONO_SUPPORT_TASKLETS)
			int restore_reg = 1;
#else
			int restore_reg = (cfg->used_int_regs & (1 << i));
#endif
			if (restore_reg) {
				amd64_mov_reg_membase (code, i, cfg->frame_reg, save_area_offset, 8);
				mono_emit_unwind_op_same_value (cfg, code, i);
				async_exc_point (code);
			}
			save_area_offset += 8;
		}
	}

	/* Load returned vtypes into registers if needed */
	cinfo = cfg->arch.cinfo;
	if (cinfo->ret.storage == ArgValuetypeInReg) {
		ArgInfo *ainfo = &cinfo->ret;
		MonoInst *inst = cfg->ret;

		for (quad = 0; quad < 2; quad ++) {
			switch (ainfo->pair_storage [quad]) {
			case ArgInIReg:
				amd64_mov_reg_membase (code, ainfo->pair_regs [quad], inst->inst_basereg, inst->inst_offset + (quad * sizeof (target_mgreg_t)), ainfo->pair_size [quad]);
				break;
			case ArgInFloatSSEReg:
				amd64_movss_reg_membase (code, ainfo->pair_regs [quad], inst->inst_basereg, inst->inst_offset + (quad * sizeof (target_mgreg_t)));
				break;
			case ArgInDoubleSSEReg:
				amd64_movsd_reg_membase (code, ainfo->pair_regs [quad], inst->inst_basereg, inst->inst_offset + (quad * sizeof (target_mgreg_t)));
				break;
			case ArgNone:
				break;
			default:
				g_assert_not_reached ();
			}
		}
	}

	if (cfg->arch.omit_fp) {
		if (cfg->arch.stack_alloc_size) {
			amd64_alu_reg_imm (code, X86_ADD, AMD64_RSP, cfg->arch.stack_alloc_size);
		}
	} else {
#ifdef TARGET_WIN32
		amd64_lea_membase (code, AMD64_RSP, AMD64_RBP, 0);
		amd64_pop_reg (code, AMD64_RBP);
		mono_emit_unwind_op_same_value (cfg, code, AMD64_RBP);
#else
		amd64_leave (code);
		mono_emit_unwind_op_same_value (cfg, code, AMD64_RBP);
#endif
	}
	mono_emit_unwind_op_def_cfa (cfg, code, AMD64_RSP, 8);
	async_exc_point (code);
	amd64_ret (code);

	/* Restore the unwind state to be the same as before the epilog */
	mono_emit_unwind_op_restore_state (cfg, code);

	set_code_cursor (cfg, code);
}

void
mono_arch_emit_exceptions (MonoCompile *cfg)
{
	MonoJumpInfo *patch_info;
	int nthrows, i;
	guint8 *code;
	MonoClass *exc_classes [16];
	guint8 *exc_throw_start [16], *exc_throw_end [16];
	guint32 code_size = 0;

	/* Compute needed space */
	for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
		if (patch_info->type == MONO_PATCH_INFO_EXC)
			code_size += 40;
		if (patch_info->type == MONO_PATCH_INFO_R8)
			code_size += 8 + 15; /* sizeof (double) + alignment */
		if (patch_info->type == MONO_PATCH_INFO_R4)
			code_size += 4 + 15; /* sizeof (float) + alignment */
		if (patch_info->type == MONO_PATCH_INFO_GC_CARD_TABLE_ADDR)
			code_size += 8 + 7; /*sizeof (void*) + alignment */
	}

	code = realloc_code (cfg, code_size);

	/* add code to raise exceptions */
	nthrows = 0;
	for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
		switch (patch_info->type) {
		case MONO_PATCH_INFO_EXC: {
			MonoClass *exc_class;
			guint8 *buf, *buf2;
			guint32 throw_ip;

			amd64_patch (patch_info->ip.i + cfg->native_code, code);

			exc_class = mono_class_load_from_name (mono_defaults.corlib, "System", patch_info->data.name);
			throw_ip = patch_info->ip.i;

			//x86_breakpoint (code);
			/* Find a throw sequence for the same exception class */
			for (i = 0; i < nthrows; ++i)
				if (exc_classes [i] == exc_class)
					break;
			if (i < nthrows) {
				amd64_mov_reg_imm (code, AMD64_ARG_REG2, (exc_throw_end [i] - cfg->native_code) - throw_ip);
				x86_jump_code (code, exc_throw_start [i]);
				patch_info->type = MONO_PATCH_INFO_NONE;
			}
			else {
				buf = code;
				amd64_mov_reg_imm_size (code, AMD64_ARG_REG2, 0xf0f0f0f0, 4);
				buf2 = code;

				if (nthrows < 16) {
					exc_classes [nthrows] = exc_class;
					exc_throw_start [nthrows] = code;
				}
				amd64_mov_reg_imm (code, AMD64_ARG_REG1, m_class_get_type_token (exc_class) - MONO_TOKEN_TYPE_DEF);

				patch_info->type = MONO_PATCH_INFO_NONE;

				code = emit_call (cfg, NULL, code, MONO_JIT_ICALL_mono_arch_throw_corlib_exception);

				amd64_mov_reg_imm (buf, AMD64_ARG_REG2, (code - cfg->native_code) - throw_ip);
				while (buf < buf2)
					x86_nop (buf);

				if (nthrows < 16) {
					exc_throw_end [nthrows] = code;
					nthrows ++;
				}
			}
			break;
		}
		default:
			/* do nothing */
			break;
		}
		set_code_cursor (cfg, code);
	}

	/* Handle relocations with RIP relative addressing */
	for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
		gboolean remove = FALSE;
		guint8 *orig_code = code;

		switch (patch_info->type) {
		case MONO_PATCH_INFO_R8:
		case MONO_PATCH_INFO_R4: {
			guint8 *pos, *patch_pos;
			guint32 target_pos;

			/* The SSE opcodes require a 16 byte alignment */
			code = (guint8*)ALIGN_TO (code, 16);

			pos = cfg->native_code + patch_info->ip.i;
			if (IS_REX (pos [1])) {
				patch_pos = pos + 5;
				target_pos = code - pos - 9;
			}
			else {
				patch_pos = pos + 4;
				target_pos = code - pos - 8;
			}

			if (patch_info->type == MONO_PATCH_INFO_R8) {
				*(double*)code = *(double*)patch_info->data.target;
				code += sizeof (double);
			} else {
				*(float*)code = *(float*)patch_info->data.target;
				code += sizeof (float);
			}

			*(guint32*)(patch_pos) = target_pos;

			remove = TRUE;
			break;
		}
		case MONO_PATCH_INFO_GC_CARD_TABLE_ADDR: {
			guint8 *pos;

			if (cfg->compile_aot)
				continue;

			/*loading is faster against aligned addresses.*/
			code = (guint8*)ALIGN_TO (code, 8);
			memset (orig_code, 0, code - orig_code);

			pos = cfg->native_code + patch_info->ip.i;

			/*alu_op [rex] modr/m imm32 - 7 or 8 bytes */
			if (IS_REX (pos [1]))
				*(guint32*)(pos + 4) = (guint8*)code - pos - 8;
			else
				*(guint32*)(pos + 3) = (guint8*)code - pos - 7;

			*(gpointer*)code = (gpointer)patch_info->data.target;
			code += sizeof (gpointer);

			remove = TRUE;
			break;
		}
		default:
			break;
		}

		if (remove) {
			if (patch_info == cfg->patch_info)
				cfg->patch_info = patch_info->next;
			else {
				MonoJumpInfo *tmp;

				for (tmp = cfg->patch_info; tmp->next != patch_info; tmp = tmp->next)
					;
				tmp->next = patch_info->next;
			}
		}
		set_code_cursor (cfg, code);
	}

	set_code_cursor (cfg, code);
}

#endif /* DISABLE_JIT */

gboolean 
mono_arch_is_inst_imm (int opcode, int imm_opcode, gint64 imm)
{
	return amd64_use_imm32 (imm);
}

guint32
mono_arch_get_patch_offset (guint8 *code)
{
	return 3;
}

#ifndef DISABLE_JIT

MonoInst*
mono_arch_emit_inst_for_method (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	MonoInst *ins = NULL;
	int opcode = 0;

	if (cmethod->klass == mono_class_try_get_math_class ()) {
		if (strcmp (cmethod->name, "Sqrt") == 0) {
			opcode = OP_SQRT;
		} else if (strcmp (cmethod->name, "Abs") == 0 && fsig->params [0]->type == MONO_TYPE_R8) {
			opcode = OP_ABS;
		}
		
		if (opcode && fsig->param_count == 1) {
			MONO_INST_NEW (cfg, ins, opcode);
			ins->type = STACK_R8;
			ins->dreg = mono_alloc_freg (cfg);
			ins->sreg1 = args [0]->dreg;
			MONO_ADD_INS (cfg->cbb, ins);
		}

		opcode = 0;
		if (cfg->opt & MONO_OPT_CMOV) {
			if (strcmp (cmethod->name, "Min") == 0) {
				if (fsig->params [0]->type == MONO_TYPE_I4)
					opcode = OP_IMIN;
				if (fsig->params [0]->type == MONO_TYPE_U4)
					opcode = OP_IMIN_UN;
				else if (fsig->params [0]->type == MONO_TYPE_I8)
					opcode = OP_LMIN;
				else if (fsig->params [0]->type == MONO_TYPE_U8)
					opcode = OP_LMIN_UN;
			} else if (strcmp (cmethod->name, "Max") == 0) {
				if (fsig->params [0]->type == MONO_TYPE_I4)
					opcode = OP_IMAX;
				if (fsig->params [0]->type == MONO_TYPE_U4)
					opcode = OP_IMAX_UN;
				else if (fsig->params [0]->type == MONO_TYPE_I8)
					opcode = OP_LMAX;
				else if (fsig->params [0]->type == MONO_TYPE_U8)
					opcode = OP_LMAX_UN;
			}
		}
		
		if (opcode && fsig->param_count == 2) {
			MONO_INST_NEW (cfg, ins, opcode);
			ins->type = fsig->params [0]->type == MONO_TYPE_I4 ? STACK_I4 : STACK_I8;
			ins->dreg = mono_alloc_ireg (cfg);
			ins->sreg1 = args [0]->dreg;
			ins->sreg2 = args [1]->dreg;
			MONO_ADD_INS (cfg->cbb, ins);
		}

#if 0
		/* OP_FREM is not IEEE compatible */
		else if (strcmp (cmethod->name, "IEEERemainder") == 0 && fsig->param_count == 2) {
			MONO_INST_NEW (cfg, ins, OP_FREM);
			ins->inst_i0 = args [0];
			ins->inst_i1 = args [1];
		}
#endif

		if ((mini_get_cpu_features (cfg) & MONO_CPU_X86_SSE41) != 0 && fsig->param_count == 1 && fsig->params [0]->type == MONO_TYPE_R8) {
			int mode = -1;
			if (!strcmp (cmethod->name, "Round"))
				mode = 0;
			else if (!strcmp (cmethod->name, "Floor"))
				mode = 1;
			else if (!strcmp (cmethod->name, "Ceiling"))
				mode = 2;
			if (mode != -1) {
				int xreg = alloc_xreg (cfg);
				EMIT_NEW_UNALU (cfg, ins, OP_FCONV_TO_R8_X, xreg, args [0]->dreg);
				EMIT_NEW_UNALU (cfg, ins, OP_SSE41_ROUNDP, xreg, xreg);
				ins->inst_c0 = mode;
				ins->inst_c1 = MONO_TYPE_R8;
				int dreg = alloc_freg (cfg);
				EMIT_NEW_UNALU (cfg, ins, OP_EXTRACT_R8, dreg, xreg);
				return ins;
			}
		}
	}

	return ins;
}
#endif

gboolean
mono_arch_opcode_supported (int opcode)
{
	switch (opcode) {
	case OP_ATOMIC_ADD_I4:
	case OP_ATOMIC_ADD_I8:
	case OP_ATOMIC_EXCHANGE_I4:
	case OP_ATOMIC_EXCHANGE_I8:
	case OP_ATOMIC_CAS_I4:
	case OP_ATOMIC_CAS_I8:
	case OP_ATOMIC_LOAD_I1:
	case OP_ATOMIC_LOAD_I2:
	case OP_ATOMIC_LOAD_I4:
	case OP_ATOMIC_LOAD_I8:
	case OP_ATOMIC_LOAD_U1:
	case OP_ATOMIC_LOAD_U2:
	case OP_ATOMIC_LOAD_U4:
	case OP_ATOMIC_LOAD_U8:
	case OP_ATOMIC_LOAD_R4:
	case OP_ATOMIC_LOAD_R8:
	case OP_ATOMIC_STORE_I1:
	case OP_ATOMIC_STORE_I2:
	case OP_ATOMIC_STORE_I4:
	case OP_ATOMIC_STORE_I8:
	case OP_ATOMIC_STORE_U1:
	case OP_ATOMIC_STORE_U2:
	case OP_ATOMIC_STORE_U4:
	case OP_ATOMIC_STORE_U8:
	case OP_ATOMIC_STORE_R4:
	case OP_ATOMIC_STORE_R8:
		return TRUE;
	default:
		return FALSE;
	}
}
