/**
 * \file
 * translator-emit.cpp: the general LLVM IR emission helpers.
 *
 * Split out of translator.cpp; see translator-internal.hpp for the shape of the
 * split and for everything shared between the pieces.
 *
 * Copyright 2009-2011 Novell Inc (http://www.novell.com)
 * Copyright 2011 Xamarin Inc (http://www.xamarin.com)
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include "translator-internal.hpp"

#ifndef DISABLE_JIT

static void set_nonnull_load_flag (LLVMValueRef v);

/*
 * get_bb:
 *
 *   Return the LLVM basic block corresponding to BB.
 */
LLVMBasicBlockRef
get_bb (EmitContext *ctx, MonoBasicBlock *bb)
{
	char bb_name_buf [128];
	char *bb_name;

	if (ctx->bblocks [bb->block_num].bblock == nullptr) {
		if (bb->flags & BB_EXCEPTION_HANDLER) {
			int clause_index = (mono_get_block_region_notry (ctx->cfg, bb->region) >> 8) - 1;
			sprintf (bb_name_buf, "EH_CLAUSE%d_BB%d", clause_index, bb->block_num);
			bb_name = bb_name_buf;
		} else if (bb->block_num < 256) {
			if (!ctx->module->bb_names) {
				ctx->module->bb_names_len = 256;
				ctx->module->bb_names = g_new0 (char*, ctx->module->bb_names_len);
			}
			if (!ctx->module->bb_names [bb->block_num]) {
				char *n;

				n = g_strdup_printf ("BB%d", bb->block_num);
				mono_memory_barrier ();
				ctx->module->bb_names [bb->block_num] = n;
			}
			bb_name = ctx->module->bb_names [bb->block_num];
		} else {
			sprintf (bb_name_buf, "BB%d", bb->block_num);
			bb_name = bb_name_buf;
		}

		ctx->bblocks [bb->block_num].bblock = LLVMAppendBasicBlock (ctx->lmethod, bb_name);
		ctx->bblocks [bb->block_num].end_bblock = ctx->bblocks [bb->block_num].bblock;
	}

	return ctx->bblocks [bb->block_num].bblock;
}

/* 
 * get_end_bb:
 *
 *   Return the last LLVM bblock corresponding to BB.
 * This might not be equal to the bb returned by get_bb () since we need to generate
 * multiple LLVM bblocks for a mono bblock to handle throwing exceptions.
 */
LLVMBasicBlockRef
get_end_bb (EmitContext *ctx, MonoBasicBlock *bb)
{
	get_bb (ctx, bb);
	return ctx->bblocks [bb->block_num].end_bblock;
}

LLVMBasicBlockRef
gen_bb (EmitContext *ctx, const char *prefix)
{
	char bb_name [128];

	sprintf (bb_name, "%s%d", prefix, ++ ctx->ex_index);
	return LLVMAppendBasicBlock (ctx->lmethod, bb_name);
}

/*
 * resolve_patch:
 *
 *   Return the target of the patch identified by TYPE and TARGET.
 */
static gpointer
resolve_patch (MonoCompile *cfg, MonoJumpInfoType type, gconstpointer target)
{
	MonoJumpInfo ji;
	ERROR_DECL (error);
	gpointer res;

	memset (&ji, 0, sizeof (ji));
	ji.type = type;
	ji.data.target = target;

	res = mono_resolve_patch_target (cfg->method, cfg->domain, NULL, &ji, FALSE, error);
	mono_error_assert_ok (error);

	return res;
}

/*
 * convert_full:
 *
 *   Emit code to convert the LLVM value V to DTYPE.
 */
LLVMValueRef
convert_full (EmitContext *ctx, LLVMValueRef v, LLVMTypeRef dtype, gboolean is_unsigned)
{
	LLVMTypeRef stype = LLVMTypeOf (v);

	if (stype != dtype) {
		bool ext = false;

		/* Extend */
		if (dtype == LLVMInt64Type () && (stype == LLVMInt32Type () || stype == LLVMInt16Type () || stype == LLVMInt8Type ()))
			ext = true;
		else if (dtype == LLVMInt32Type () && (stype == LLVMInt16Type () || stype == LLVMInt8Type ()))
			ext = true;
		else if (dtype == LLVMInt16Type () && (stype == LLVMInt8Type ()))
			ext = true;

		if (ext)
			return is_unsigned ? LLVMBuildZExt (ctx->builder, v, dtype, "") : LLVMBuildSExt (ctx->builder, v, dtype, "");

		if (dtype == LLVMDoubleType () && stype == LLVMFloatType ())
			return LLVMBuildFPExt (ctx->builder, v, dtype, "");

		/* Trunc */
		if (stype == LLVMInt64Type () && (dtype == LLVMInt32Type () || dtype == LLVMInt16Type () || dtype == LLVMInt8Type ()))
			return LLVMBuildTrunc (ctx->builder, v, dtype, "");
		if (stype == LLVMInt32Type () && (dtype == LLVMInt16Type () || dtype == LLVMInt8Type ()))
			return LLVMBuildTrunc (ctx->builder, v, dtype, "");
		if (stype == LLVMInt16Type () && dtype == LLVMInt8Type ())
			return LLVMBuildTrunc (ctx->builder, v, dtype, "");
		if (stype == LLVMDoubleType () && dtype == LLVMFloatType ())
			return LLVMBuildFPTrunc (ctx->builder, v, dtype, "");

		if (LLVMGetTypeKind (stype) == LLVMPointerTypeKind && LLVMGetTypeKind (dtype) == LLVMPointerTypeKind)
			return LLVMBuildBitCast (ctx->builder, v, dtype, "");
		if (LLVMGetTypeKind (dtype) == LLVMPointerTypeKind)
			return LLVMBuildIntToPtr (ctx->builder, v, dtype, "");
		if (LLVMGetTypeKind (stype) == LLVMPointerTypeKind)
			return LLVMBuildPtrToInt (ctx->builder, v, dtype, "");

		if (mono_arch_is_soft_float ()) {
			if (stype == LLVMInt32Type () && dtype == LLVMFloatType ())
				return LLVMBuildBitCast (ctx->builder, v, dtype, "");
			if (stype == LLVMInt32Type () && dtype == LLVMDoubleType ())
				return LLVMBuildBitCast (ctx->builder, LLVMBuildZExt (ctx->builder, v, LLVMInt64Type (), ""), dtype, "");
		}

		if (LLVMGetTypeKind (stype) == LLVMVectorTypeKind && LLVMGetTypeKind (dtype) == LLVMVectorTypeKind)
			return LLVMBuildBitCast (ctx->builder, v, dtype, "");

		mono_llvm_dump_value (v);
		mono_llvm_dump_type (dtype);
		printf ("\n");
		g_assert_not_reached ();
		return nullptr;
	} else {
		return v;
	}
}

LLVMValueRef
convert (EmitContext *ctx, LLVMValueRef v, LLVMTypeRef dtype)
{
	return convert_full (ctx, v, dtype, FALSE);
}

void
emit_memset (EmitContext *ctx, LLVMBuilderRef builder, LLVMValueRef v, LLVMValueRef size, int alignment)
{
	LLVMValueRef args [5];
	int aindex = 0;

	args [aindex ++] = v;
	args [aindex ++] = LLVMConstInt (LLVMInt8Type (), 0, FALSE);
	args [aindex ++] = size;
#if LLVM_API_VERSION < 900
	args [aindex ++] = LLVMConstInt (LLVMInt32Type (), alignment, FALSE);
#endif
	args [aindex ++] = LLVMConstInt (LLVMInt1Type (), 0, FALSE);
	LLVMBuildCall2 (builder, LLVMGlobalGetValueType (get_intrins (ctx, INTRINS_MEMSET)), get_intrins (ctx, INTRINS_MEMSET), args, aindex, "");
}

/*
 * emit_volatile_load:
 *
 *   If vreg is volatile, emit a load from its address.
 */
LLVMValueRef
emit_volatile_load (EmitContext *ctx, int vreg)
{
	MonoType *t;
	LLVMValueRef v;

	// On arm64, we pass the rgctx in a callee saved
	// register on arm64 (x15), and llvm might keep the value in that register
	// even through the register is marked as 'reserved' inside llvm.

	v = mono_llvm_build_load (ctx->builder, ctx->addresses [vreg]->type, ctx->addresses [vreg]->value, "", TRUE);
	t = ctx->vreg_cli_types [vreg];
	if (t && !t->byref) {
		/* 
		 * Might have to zero extend since llvm doesn't have 
		 * unsigned types.
		 */
		if (t->type == MONO_TYPE_U1 || t->type == MONO_TYPE_U2 || t->type == MONO_TYPE_CHAR || t->type == MONO_TYPE_BOOLEAN)
			v = LLVMBuildZExt (ctx->builder, v, LLVMInt32Type (), "");
		else if (t->type == MONO_TYPE_I1 || t->type == MONO_TYPE_I2)
			v = LLVMBuildSExt (ctx->builder, v, LLVMInt32Type (), "");
		else if (t->type == MONO_TYPE_U8)
			v = LLVMBuildZExt (ctx->builder, v, LLVMInt64Type (), "");
	}

	return v;
}

/*
 * emit_volatile_store:
 *
 *   If VREG is volatile, emit a store from its value to its address.
 */
void
emit_volatile_store (EmitContext *ctx, int vreg)
{
	MonoInst *var = get_vreg_to_inst (ctx->cfg, vreg);

	if (var && var->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT)) {
		g_assert (ctx->addresses [vreg]);
		LLVMBuildStore (ctx->builder, convert (ctx, ctx->values [vreg], type_to_llvm_type (ctx, var->inst_vtype)), ctx->addresses [vreg]->value);
	}
}

static LLVMTypeRef
sig_to_llvm_sig_no_cinfo (EmitContext *ctx, MonoMethodSignature *sig)
{
	LLVMTypeRef ret_type;
	LLVMTypeRef *param_types = nullptr;
	LLVMTypeRef res;
	int i, pindex;
	MonoType *rtype;

	ret_type = type_to_llvm_type (ctx, sig->ret);
	if (!ctx_ok (ctx))
		return nullptr;
	rtype = mini_get_underlying_type (sig->ret);

	param_types = g_new0 (LLVMTypeRef, (sig->param_count * 8) + 3);
	pindex = 0;

	if (sig->hasthis)
		param_types [pindex ++] = ThisType ();
	for (i = 0; i < sig->param_count; ++i)
		param_types [pindex ++] = type_to_llvm_arg_type (ctx, sig->params [i]);

	if (!ctx_ok (ctx)) {
		g_free (param_types);
		return nullptr;
	}

	res = LLVMFunctionType (ret_type, param_types, pindex, FALSE);
	g_free (param_types);

	return res;
}

/*
 * sig_to_llvm_sig_full:
 *
 *   Return the LLVM signature corresponding to the mono signature SIG using the
 * calling convention information in CINFO. Fill out the parameter mapping information in CINFO.
 */
LLVMTypeRef
sig_to_llvm_sig_full (EmitContext *ctx, MonoMethodSignature *sig, LLVMCallInfo *cinfo)
{
	LLVMTypeRef ret_type;
	LLVMTypeRef *param_types = nullptr;
	LLVMTypeRef res;
	int i, j, pindex, vret_arg_pindex = 0;
	bool vretaddr = false;
	MonoType *rtype;

	if (!cinfo)
		return sig_to_llvm_sig_no_cinfo (ctx, sig);

	ret_type = type_to_llvm_type (ctx, sig->ret);
	if (!ctx_ok (ctx))
		return nullptr;
	rtype = mini_get_underlying_type (sig->ret);

	switch (cinfo->ret.storage) {
	case LLVMArgVtypeInReg:
		/* LLVM models this by returning an aggregate value */
		if (cinfo->ret.pair_storage [0] == LLVMArgInIReg && cinfo->ret.pair_storage [1] == LLVMArgNone) {
			LLVMTypeRef members [2];

			members [0] = IntPtrType ();
			ret_type = LLVMStructType (members, 1, FALSE);
		} else if (cinfo->ret.pair_storage [0] == LLVMArgNone && cinfo->ret.pair_storage [1] == LLVMArgNone) {
			/* Empty struct */
			ret_type = LLVMVoidType ();
		} else if (cinfo->ret.pair_storage [0] == LLVMArgInIReg && cinfo->ret.pair_storage [1] == LLVMArgInIReg) {
			LLVMTypeRef members [2];

			members [0] = IntPtrType ();
			members [1] = IntPtrType ();
			ret_type = LLVMStructType (members, 2, FALSE);
		} else {
			g_assert_not_reached ();
		}
		break;
	case LLVMArgVtypeByVal:
		/* Vtype returned normally by val */
		break;
	case LLVMArgVtypeAsScalar: {
		int size = mono_class_value_size (mono_class_from_mono_type_internal (rtype), NULL);
		/* LLVM models this by returning an int */
		if (size < TARGET_SIZEOF_VOID_P) {
			g_assert (cinfo->ret.nslots == 1);
			ret_type = LLVMIntType (size * 8);
		} else {
			g_assert (cinfo->ret.nslots == 1 || cinfo->ret.nslots == 2);
			ret_type = LLVMIntType (cinfo->ret.nslots * sizeof (target_mgreg_t) * 8);
		}
		break;
	}
	case LLVMArgAsIArgs:
		ret_type = LLVMArrayType (IntPtrType (), cinfo->ret.nslots);
		break;
	case LLVMArgFpStruct: {
		/* Vtype returned as a fp struct */
		LLVMTypeRef members [16];

		/* Have to create our own structure since we don't map fp structures to LLVM fp structures yet */
		for (i = 0; i < cinfo->ret.nslots; ++i)
			members [i] = cinfo->ret.esize == 8 ? LLVMDoubleType () : LLVMFloatType ();
		ret_type = LLVMStructType (members, cinfo->ret.nslots, FALSE);
		break;
	}
	case LLVMArgVtypeByRef:
		/* Vtype returned using a hidden argument */
		ret_type = LLVMVoidType ();
		break;
	case LLVMArgVtypeRetAddr:
	case LLVMArgGsharedvtFixed:
	case LLVMArgGsharedvtFixedVtype:
	case LLVMArgGsharedvtVariable:
		vretaddr = true;
		ret_type = LLVMVoidType ();
		break;
	default:
		break;
	}

	param_types = g_new0 (LLVMTypeRef, (sig->param_count * 8) + 3);
	pindex = 0;
	if (cinfo->ret.storage == LLVMArgVtypeByRef) {
		/*
		 * Has to be the first argument because of the sret argument attribute
		 * FIXME: This might conflict with passing 'this' as the first argument, but
		 * this is only used on arm64 which has a dedicated struct return register.
		 */
		cinfo->vret_arg_pindex = pindex;
		param_types [pindex] = type_to_llvm_arg_type (ctx, sig->ret);
		if (!ctx_ok (ctx)) {
			g_free (param_types);
			return nullptr;
		}
		param_types [pindex] = LLVMPointerType (param_types [pindex], 0);
		pindex ++;
	}
	if (cinfo->rgctx_arg) {
		cinfo->rgctx_arg_pindex = pindex;
		param_types [pindex] = ctx->module->ptr_type;
		pindex ++;
	}
	if (cinfo->imt_arg) {
		cinfo->imt_arg_pindex = pindex;
		param_types [pindex] = ctx->module->ptr_type;
		pindex ++;
	}
	if (vretaddr) {
		/* Compute the index in the LLVM signature where the vret arg needs to be passed */
		vret_arg_pindex = pindex;
		if (cinfo->vret_arg_index == 1) {
			/* Add the slots consumed by the first argument */
			LLVMArgInfo *ainfo = &cinfo->args [0];
			switch (ainfo->storage) {
			case LLVMArgVtypeInReg:
				for (j = 0; j < 2; ++j) {
					if (ainfo->pair_storage [j] == LLVMArgInIReg)
						vret_arg_pindex ++;
				}
				break;
			default:
				vret_arg_pindex ++;
			}
		}

		cinfo->vret_arg_pindex = vret_arg_pindex;
	}				

	if (vretaddr && vret_arg_pindex == pindex)
		param_types [pindex ++] = IntPtrType ();
	if (sig->hasthis) {
		cinfo->this_arg_pindex = pindex;
		param_types [pindex ++] = ThisType ();
		cinfo->args [0].pindex = cinfo->this_arg_pindex;
	}
	if (vretaddr && vret_arg_pindex == pindex)
		param_types [pindex ++] = IntPtrType ();
	for (i = 0; i < sig->param_count; ++i) {
		LLVMArgInfo *ainfo = &cinfo->args [i + sig->hasthis];

		if (vretaddr && vret_arg_pindex == pindex)
			param_types [pindex ++] = IntPtrType ();
		ainfo->pindex = pindex;

		switch (ainfo->storage) {
		case LLVMArgVtypeInReg:
			for (j = 0; j < 2; ++j) {
				switch (ainfo->pair_storage [j]) {
				case LLVMArgInIReg:
					param_types [pindex ++] = LLVMIntType (TARGET_SIZEOF_VOID_P * 8);
					break;
				case LLVMArgNone:
					break;
				default:
					g_assert_not_reached ();
				}
			}
			break;
		case LLVMArgVtypeByVal:
			param_types [pindex] = type_to_llvm_arg_type (ctx, ainfo->type);
			if (!ctx_ok (ctx))
				break;
			param_types [pindex] = LLVMPointerType (param_types [pindex], 0);
			pindex ++;
			break;
		case LLVMArgAsIArgs:
			if (ainfo->esize == 8)
				param_types [pindex] = LLVMArrayType (LLVMInt64Type (), ainfo->nslots);
			else
				param_types [pindex] = LLVMArrayType (IntPtrType (), ainfo->nslots);
			pindex ++;
			break;
		case LLVMArgVtypeAddr:
		case LLVMArgVtypeByRef:
			param_types [pindex] = type_to_llvm_arg_type (ctx, ainfo->type);
			if (!ctx_ok (ctx))
				break;
			param_types [pindex] = LLVMPointerType (param_types [pindex], 0);
			pindex ++;
			break;
		case LLVMArgAsFpArgs: {
			int j;

			/* Emit dummy fp arguments if needed so the rest is passed on the stack */
			for (j = 0; j < ainfo->ndummy_fpargs; ++j)
				param_types [pindex ++] = LLVMDoubleType ();
			for (j = 0; j < ainfo->nslots; ++j)
				param_types [pindex ++] = ainfo->esize == 8 ? LLVMDoubleType () : LLVMFloatType ();
			break;
		}
		case LLVMArgVtypeAsScalar:
			g_assert_not_reached ();
			break;
		case LLVMArgGsharedvtFixed:
		case LLVMArgGsharedvtFixedVtype:
			param_types [pindex ++] = LLVMPointerType (type_to_llvm_arg_type (ctx, ainfo->type), 0);
			break;
		case LLVMArgGsharedvtVariable:
			param_types [pindex ++] = LLVMPointerType (IntPtrType (), 0);
			break;
		default:
			param_types [pindex ++] = type_to_llvm_arg_type (ctx, ainfo->type);
			break;
		}
	}
	if (!ctx_ok (ctx)) {
		g_free (param_types);
		return nullptr;
	}
	if (vretaddr && vret_arg_pindex == pindex)
		param_types [pindex ++] = IntPtrType ();
	res = LLVMFunctionType (ret_type, param_types, pindex, FALSE);
	g_free (param_types);

	return res;
}

LLVMTypeRef
sig_to_llvm_sig (EmitContext *ctx, MonoMethodSignature *sig)
{
	return sig_to_llvm_sig_full (ctx, sig, NULL);
}

/*
 * LLVMFunctionType1:
 *
 *   Create an LLVM function type from the arguments.
 */
G_GNUC_UNUSED LLVMTypeRef
LLVMFunctionType0 (LLVMTypeRef ReturnType,
				   int IsVarArg)
{
	return LLVMFunctionType (ReturnType, NULL, 0, IsVarArg);
}

/*
 * LLVMFunctionType1:
 *
 *   Create an LLVM function type from the arguments.
 */
static G_GNUC_UNUSED LLVMTypeRef 
LLVMFunctionType1 (LLVMTypeRef ReturnType,
				   LLVMTypeRef ParamType1,
				   int IsVarArg)
{
	LLVMTypeRef param_types [1];

	param_types [0] = ParamType1;

	return LLVMFunctionType (ReturnType, param_types, 1, IsVarArg);
}

/*
 * LLVMFunctionType2:
 *
 *   Create an LLVM function type from the arguments.
 */
static G_GNUC_UNUSED LLVMTypeRef
LLVMFunctionType2 (LLVMTypeRef ReturnType,
				   LLVMTypeRef ParamType1,
				   LLVMTypeRef ParamType2,
				   int IsVarArg)
{
	LLVMTypeRef param_types [2];

	param_types [0] = ParamType1;
	param_types [1] = ParamType2;

	return LLVMFunctionType (ReturnType, param_types, 2, IsVarArg);
}

/*
 * create_builder:
 *
 *   Create an LLVM builder and remember it so it can be freed later.
 */
LLVMBuilderRef
create_builder (EmitContext *ctx)
{
	LLVMBuilderRef builder = LLVMCreateBuilder ();
	if (mono_use_fast_math)
		mono_llvm_set_fast_math (builder);

	ctx->builders = g_slist_prepend_mempool (ctx->cfg->mempool, ctx->builders, builder);

	return builder;
}

static char*
get_aotconst_name (MonoJumpInfoType type, gconstpointer data, int got_offset)
{
	char *name;
	int len;

	switch (type) {
	case MONO_PATCH_INFO_JIT_ICALL_ID:
		name = g_strdup_printf ("jit_icall_%s", mono_find_jit_icall_info (static_cast<MonoJitICallId>(reinterpret_cast<gsize>(data)))->name);
		break;
	case MONO_PATCH_INFO_JIT_ICALL_ADDR_NOCALL:
		name = g_strdup_printf ("jit_icall_addr_nocall_%s", mono_find_jit_icall_info (static_cast<MonoJitICallId>(reinterpret_cast<gsize>(data)))->name);
		break;
	case MONO_PATCH_INFO_RGCTX_SLOT_INDEX: {
		MonoJumpInfoRgctxEntry *entry = (MonoJumpInfoRgctxEntry*)data;
		name = g_strdup_printf ("rgctx_slot_index_%s", mono_rgctx_info_type_to_str (entry->info_type));
		break;
	}
	case MONO_PATCH_INFO_AOT_MODULE:
	case MONO_PATCH_INFO_GC_SAFE_POINT_FLAG:
	case MONO_PATCH_INFO_GC_CARD_TABLE_ADDR:
	case MONO_PATCH_INFO_GC_NURSERY_START:
	case MONO_PATCH_INFO_GC_NURSERY_BITS:
	case MONO_PATCH_INFO_INTERRUPTION_REQUEST_FLAG:
		name = g_strdup_printf ("%s", mono_ji_type_to_string (type));
		len = strlen (name);
		for (int i = 0; i < len; ++i)
			name [i] = tolower (name [i]);
		break;
	default:
		name = g_strdup_printf ("%s_%d", mono_ji_type_to_string (type), got_offset);
		len = strlen (name);
		for (int i = 0; i < len; ++i)
			name [i] = tolower (name [i]);
		break;
	}

	return name;
}

static int
compute_aot_got_offset (MonoLLVMModule *module, MonoJumpInfo *ji, LLVMTypeRef llvm_type)
{
	guint32 got_offset = mono_aot_get_got_offset (ji);

	LLVMTypeRef lookup_type = static_cast<LLVMTypeRef>(g_hash_table_lookup (module->got_idx_to_type, GINT_TO_POINTER (got_offset)));

	if (!lookup_type) {
		lookup_type = llvm_type;
	} else if (llvm_type != lookup_type) {
		lookup_type = module->ptr_type;
	} else {
		return got_offset;
	}

	g_hash_table_insert (module->got_idx_to_type, GINT_TO_POINTER (got_offset), lookup_type);
	return got_offset;
}

/* Allocate a GOT slot for TYPE/DATA, and emit IR to load it */
static LLVMValueRef
get_aotconst_module (MonoLLVMModule *module, LLVMBuilderRef builder, MonoJumpInfoType type, gconstpointer data, LLVMTypeRef llvm_type,
					 guint32 *out_got_offset, MonoJumpInfo **out_ji)
{
	guint32 got_offset;
	LLVMValueRef load;

	MonoJumpInfo tmp_ji;
	tmp_ji.type = type;
	tmp_ji.data.target = data;

	MonoJumpInfo *ji = mono_aot_patch_info_dup (&tmp_ji);

	if (out_ji)
		*out_ji = ji;

	got_offset = compute_aot_got_offset (module, ji, llvm_type);
	module->max_got_offset = MAX (module->max_got_offset, got_offset);

	if (out_got_offset)
		*out_got_offset = got_offset;

	LLVMValueRef const_var = static_cast<LLVMValueRef>(g_hash_table_lookup (module->aotconst_vars, GINT_TO_POINTER (got_offset)));
	if (!const_var) {
		LLVMTypeRef type = llvm_type;
		// FIXME:
		char *name = get_aotconst_name (ji->type, ji->data.target, got_offset);
		char *symbol = g_strdup_printf ("aotconst_%s", name);
		g_free (name);
		LLVMValueRef v = LLVMAddGlobal (module->lmodule, type, symbol);
		LLVMSetVisibility (v, LLVMHiddenVisibility);
		LLVMSetLinkage (v, LLVMInternalLinkage);
		LLVMSetInitializer (v, LLVMConstNull (type));
		// FIXME:
		LLVMSetAlignment (v, 8);

		g_hash_table_insert (module->aotconst_vars, GINT_TO_POINTER (got_offset), v);
		const_var = v;
	}

	load = LLVMBuildLoad2 (builder, llvm_type, const_var, "");

	if (mono_aot_is_shared_got_offset (got_offset))
		set_invariant_load_flag (load);
	if (type == MONO_PATCH_INFO_LDSTR)
		set_nonnull_load_flag (load);

	load = LLVMBuildBitCast (builder, load, llvm_type, "");

	return load;
}

LLVMValueRef
get_aotconst (EmitContext *ctx, MonoJumpInfoType type, gconstpointer data, LLVMTypeRef llvm_type)
{
	MonoCompile *cfg;
	guint32 got_offset;
	MonoJumpInfo *ji;
	LLVMValueRef load;

	cfg = ctx->cfg;

	load = get_aotconst_module (ctx->module, ctx->builder, type, data, llvm_type, &got_offset, &ji);

	ji->next = cfg->patch_info;
	cfg->patch_info = ji;

	/* 
	 * If the got slot is shared, it means its initialized when the aot image is loaded, so we don't need to
	 * explicitly initialize it.
	 */
	if (!mono_aot_is_shared_got_offset (got_offset)) {
		//mono_print_ji (ji);
		//printf ("\n");
		ctx->cfg->got_access_count ++;
	}

	return load;
}

LLVMValueRef
get_jit_callee (EmitContext *ctx, const char *name, LLVMTypeRef llvm_sig, MonoJumpInfoType type, gconstpointer data)
{
	gpointer target;

	// This won't be patched so compile the wrapper immediately
	if (type == MONO_PATCH_INFO_JIT_ICALL_ID) {
		MonoJitICallInfo * const info = mono_find_jit_icall_info (static_cast<MonoJitICallId>(reinterpret_cast<gsize>(data)));
		target = const_cast<gpointer>(mono_icall_get_wrapper_full (info, TRUE));
	} else {
		target = resolve_patch (ctx->cfg, type, data);
	}

	LLVMValueRef tramp_var = LLVMAddGlobal (ctx->lmodule, LLVMPointerType (llvm_sig, 0), name);
	LLVMSetInitializer (tramp_var, LLVMConstIntToPtr (LLVMConstInt (LLVMInt64Type (), static_cast<guint64>(reinterpret_cast<size_t>(target)), FALSE), LLVMPointerType (llvm_sig, 0)));
	LLVMSetLinkage (tramp_var, LLVMExternalLinkage);
	LLVMValueRef callee = LLVMBuildLoad2 (ctx->builder, LLVMPointerType (llvm_sig, 0), tramp_var, "");
	return callee;
}

static int
get_handler_clause (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoMethodHeader *header = cfg->header;
	MonoExceptionClause *clause;
	int i;

	/* Directly */
	if (bb->region != (guint)-1 && MONO_BBLOCK_IS_IN_REGION (bb, MONO_REGION_TRY))
		return (bb->region >> 8) - 1;

	/* Indirectly */
	for (i = 0; i < header->num_clauses; ++i) {
		clause = &header->clauses [i];
			   
		if (MONO_OFFSET_IN_CLAUSE (clause, bb->real_offset) && clause->flags == MONO_EXCEPTION_CLAUSE_NONE)
			return i;
	}

	return -1;
}

	
void
set_metadata_flag (LLVMValueRef v, const char *flag_name)
{
	LLVMValueRef md_arg;
	int md_kind;

	md_kind = LLVMGetMDKindID (flag_name, strlen (flag_name));
	md_arg = LLVMMDString ("mono", 4);
	LLVMSetMetadata (v, md_kind, LLVMMDNode (&md_arg, 1));
}

static void
set_nonnull_load_flag (LLVMValueRef v)
{
	LLVMValueRef md_arg;
	int md_kind;
	const char *flag_name;

	flag_name = "nonnull";
	md_kind = LLVMGetMDKindID (flag_name, strlen (flag_name));
	md_arg = LLVMMDString ("<index>", strlen ("<index>"));
	LLVMSetMetadata (v, md_kind, LLVMMDNode (&md_arg, 1));
}

void
set_nontemporal_flag (LLVMValueRef v)
{
	LLVMValueRef md_arg;
	int md_kind;
	const char *flag_name;

	// FIXME: Cache this
	flag_name = "nontemporal";
	md_kind = LLVMGetMDKindID (flag_name, strlen (flag_name));
	md_arg = const_int32 (1);
	LLVMSetMetadata (v, md_kind, LLVMMDNode (&md_arg, 1));
}

void
set_invariant_load_flag (LLVMValueRef v)
{
	LLVMValueRef md_arg;
	int md_kind;
	const char *flag_name;

	// FIXME: Cache this
	flag_name = "invariant.load";
	md_kind = LLVMGetMDKindID (flag_name, strlen (flag_name));
	md_arg = LLVMMDString ("<index>", strlen ("<index>"));
	LLVMSetMetadata (v, md_kind, LLVMMDNode (&md_arg, 1));
}

/*
 * emit_call:
 *
 *   Emit an LLVM call or invoke instruction depending on whenever the call is inside
 * a try region.
 */
LLVMValueRef
emit_call (EmitContext *ctx, MonoBasicBlock *bb, LLVMBuilderRef *builder_ref, LLVMTypeRef sig, LLVMValueRef callee, LLVMValueRef *args, int pindex)
{
	MonoCompile *cfg = ctx->cfg;
	LLVMValueRef lcall = nullptr;
	LLVMBuilderRef builder = *builder_ref;
	{
		int clause_index = get_handler_clause (cfg, bb);

		if (clause_index != -1) {
			MonoMethodHeader *header = cfg->header;
			MonoExceptionClause *ec = &header->clauses [clause_index];
			MonoBasicBlock *tblock;
			LLVMBasicBlockRef ex_bb, noex_bb;

			/*
			 * Have to use an invoke instead of a call, branching to the
			 * handler bblock of the clause containing this bblock.
			 */

			g_assert (ec->flags == MONO_EXCEPTION_CLAUSE_NONE || ec->flags == MONO_EXCEPTION_CLAUSE_FINALLY || ec->flags == MONO_EXCEPTION_CLAUSE_FAULT);

			tblock = cfg->cil_offset_to_bb [ec->handler_offset];
			g_assert (tblock);

			ctx->bblocks [tblock->block_num].invoke_target = TRUE;

			ex_bb = get_bb (ctx, tblock);

			noex_bb = gen_bb (ctx, "NOEX_BB");

			/* Use an invoke */
			lcall = LLVMBuildInvoke2 (builder, sig, callee, args, pindex, noex_bb, ex_bb, "");

			builder = ctx->builder = create_builder (ctx);
			LLVMPositionBuilderAtEnd (ctx->builder, noex_bb);

			ctx->bblocks [bb->block_num].end_bblock = noex_bb;
		}
	}
	
	if (!lcall) {
		lcall = LLVMBuildCall2 (builder, sig, callee, args, pindex, "");
		ctx->builder = builder;
	}

	if (builder_ref)
		*builder_ref = ctx->builder;

	return lcall;
}

LLVMValueRef
emit_load (EmitContext *ctx, MonoBasicBlock *bb, LLVMBuilderRef *builder_ref, int size, LLVMTypeRef type, LLVMValueRef addr, LLVMValueRef base, const char *name, gboolean is_faulting, gboolean is_volatile, BarrierKind barrier)
{
	LLVMValueRef res;

	/* 
	 * We emit volatile loads for loads which can fault, because otherwise
	 * LLVM will generate invalid code when encountering a load from a
	 * NULL address.
	 */
	if (barrier != LLVM_BARRIER_NONE)
		res = mono_llvm_build_atomic_load (*builder_ref, type, addr, name, is_volatile, size, barrier);
	else
		res = mono_llvm_build_load (*builder_ref, type, addr, name, is_volatile);

	return res;
}

void
emit_store_general (EmitContext *ctx, MonoBasicBlock *bb, LLVMBuilderRef *builder_ref, int size, LLVMValueRef value, LLVMValueRef addr, LLVMValueRef base, gboolean is_faulting, gboolean is_volatile, BarrierKind barrier)
{
	if (barrier != LLVM_BARRIER_NONE)
		mono_llvm_build_aligned_store (*builder_ref, value, addr, barrier, size);
	else
		mono_llvm_build_store (*builder_ref, value, addr, is_volatile, barrier);
}

void
emit_store (EmitContext *ctx, MonoBasicBlock *bb, LLVMBuilderRef *builder_ref, int size, LLVMValueRef value, LLVMValueRef addr, LLVMValueRef base, gboolean is_faulting, gboolean is_volatile)
{
	emit_store_general (ctx, bb, builder_ref, size, value, addr, base, is_faulting, is_volatile, LLVM_BARRIER_NONE);
}

/*
 * emit_cond_system_exception:
 *
 *   Emit code to throw the exception EXC_TYPE if the condition CMP is false.
 * Might set the ctx exception.
 */
void
emit_cond_system_exception (EmitContext *ctx, MonoBasicBlock *bb, const char *exc_type, LLVMValueRef cmp, gboolean force_explicit)
{
	LLVMBasicBlockRef ex_bb, ex2_bb = nullptr, noex_bb;
	LLVMBuilderRef builder;
	MonoClass *exc_class;
	LLVMValueRef args [2];
	LLVMValueRef callee;
	bool no_pc = false;
	static MonoClass *exc_classes [MONO_EXC_INTRINS_NUM];

	if constexpr (IS_TARGET_AMD64)
		/* Some platforms don't require the pc argument */
		no_pc = true;

	int exc_id = mini_exception_id_by_name (exc_type);
	if (!exc_classes [exc_id])
		exc_classes [exc_id] = mono_class_load_from_name (mono_get_corlib (), "System", exc_type);
	exc_class = exc_classes [exc_id];
	
	ex_bb = gen_bb (ctx, "EX_BB");
	noex_bb = gen_bb (ctx, "NOEX_BB");

	LLVMValueRef branch = LLVMBuildCondBr (ctx->builder, cmp, ex_bb, noex_bb);
	if (exc_id == MONO_EXC_NULL_REF && !ctx->cfg->disable_llvm_implicit_null_checks && !force_explicit) {
		mono_llvm_set_implicit_branch (ctx->builder, branch);
	}

	/* Emit exception throwing code */
	ctx->builder = builder = create_builder (ctx);
	LLVMPositionBuilderAtEnd (builder, ex_bb);

	callee = ctx->module->throw_corlib_exception;

	/*
	 * Hoisted out of the !callee branch: emit_call () needs the signature the
	 * callee was declared with, and the callee may come from the module cache.
	 * Never derive it from the callee value itself.
	 */
	LLVMTypeRef sig;
	if (no_pc)
		sig = LLVMFunctionType1 (LLVMVoidType (), LLVMInt32Type (), FALSE);
	else
		sig = LLVMFunctionType2 (LLVMVoidType (), LLVMInt32Type (), LLVMPointerType (LLVMInt8Type (), 0), FALSE);

	if (!callee) {
		const MonoJitICallId icall_id = MONO_JIT_ICALL_mono_llvm_throw_corlib_exception_abs_trampoline;

		{
			/*
			 * Differences between the LLVM/non-LLVM throw corlib exception trampoline:
			 * - On x86, LLVM generated code doesn't push the arguments
			 * - The trampoline takes the throw address as an arguments, not a pc offset.
			 */
			callee = get_jit_callee (ctx, "llvm_throw_corlib_exception_trampoline", sig, MONO_PATCH_INFO_JIT_ICALL_ID, GUINT_TO_POINTER (icall_id));

			/*
			 * Make sure that ex_bb starts with the invoke, so the block address points to it, and not to the load 
			 * added by get_jit_callee ().
			 */
			ex2_bb = gen_bb (ctx, "EX2_BB");
			LLVMBuildBr (builder, ex2_bb);
			ex_bb = ex2_bb;

			ctx->builder = builder = create_builder (ctx);
			LLVMPositionBuilderAtEnd (ctx->builder, ex2_bb);
		}
	}

	args [0] = LLVMConstInt (LLVMInt32Type (), m_class_get_type_token (exc_class) - MONO_TOKEN_TYPE_DEF, FALSE);

	/*
	 * The LLVM mono branch contains changes so a block address can be passed as an
	 * argument to a call.
	 */
	if (no_pc) {
		emit_call (ctx, bb, &builder, sig, callee, args, 1);
	} else {
		args [1] = LLVMBlockAddress (ctx->lmethod, ex_bb);
		emit_call (ctx, bb, &builder, sig, callee, args, 2);
	}

	LLVMBuildUnreachable (builder);

	ctx->builder = builder = create_builder (ctx);
	LLVMPositionBuilderAtEnd (ctx->builder, noex_bb);

	ctx->bblocks [bb->block_num].end_bblock = noex_bb;

	ctx->ex_index ++;
	return;
}

/*
 * emit_args_to_vtype:
 *
 *   Emit code to store the vtype in the arguments args to the address ADDRESS.
 */
void
emit_args_to_vtype (EmitContext *ctx, LLVMBuilderRef builder, MonoType *t, LLVMValueRef address, LLVMArgInfo *ainfo, LLVMValueRef *args)
{
	int j, size, nslots;
	MonoClass *klass;

	t = mini_get_underlying_type (t);
	klass = mono_class_from_mono_type_internal (t);
	size = mono_class_value_size (klass, NULL);

	if (MONO_CLASS_IS_SIMD (ctx->cfg, klass))
		address = LLVMBuildBitCast (ctx->builder, address, LLVMPointerType (LLVMInt8Type (), 0), "");

	if (ainfo->storage == LLVMArgAsFpArgs)
		nslots = ainfo->nslots;
	else
		nslots = 2;

	for (j = 0; j < nslots; ++j) {
		LLVMValueRef index [2], addr, daddr;
		int part_size = size > TARGET_SIZEOF_VOID_P ? TARGET_SIZEOF_VOID_P : size;
		LLVMTypeRef part_type;

		while (part_size != 1 && part_size != 2 && part_size != 4 && part_size < 8)
			part_size ++;

		if (ainfo->pair_storage [j] == LLVMArgNone)
			continue;

		switch (ainfo->pair_storage [j]) {
		case LLVMArgInIReg: {
			part_type = LLVMIntType (part_size * 8);
			if (MONO_CLASS_IS_SIMD (ctx->cfg, klass)) {
				index [0] = LLVMConstInt (LLVMInt32Type (), j * TARGET_SIZEOF_VOID_P, FALSE);
				addr = LLVMBuildGEP2 (builder, LLVMInt8Type (), address, index, 1, "");
			} else {
				daddr = LLVMBuildBitCast (ctx->builder, address, LLVMPointerType (IntPtrType (), 0), "");
				index [0] = LLVMConstInt (LLVMInt32Type (), j, FALSE);
				addr = LLVMBuildGEP2 (builder, IntPtrType (), daddr, index, 1, "");
			}
			LLVMBuildStore (builder, convert (ctx, args [j], part_type), LLVMBuildBitCast (ctx->builder, addr, LLVMPointerType (part_type, 0), ""));
			break;
		}
		case LLVMArgInFPReg: {
			LLVMTypeRef arg_type;

			if (ainfo->esize == 8)
				arg_type = LLVMDoubleType ();
			else
				arg_type = LLVMFloatType ();

			index [0] = LLVMConstInt (LLVMInt32Type (), j, FALSE);
			daddr = LLVMBuildBitCast (ctx->builder, address, LLVMPointerType (arg_type, 0), "");
			addr = LLVMBuildGEP2 (builder, arg_type, daddr, index, 1, "");
			LLVMBuildStore (builder, args [j], addr);
			break;
		}
		case LLVMArgNone:
			break;
		default:
			g_assert_not_reached ();
		}

		size -= TARGET_SIZEOF_VOID_P;
	}
}

/*
 * emit_vtype_to_args:
 *
 *   Emit code to load a vtype at address ADDRESS into scalar arguments. Store the arguments
 * into ARGS, and the number of arguments into NARGS.
 */
void
emit_vtype_to_args (EmitContext *ctx, LLVMBuilderRef builder, MonoType *t, LLVMValueRef address, LLVMArgInfo *ainfo, LLVMValueRef *args, guint32 *nargs)
{
	int pindex = 0;
	int j, size, nslots;
	LLVMTypeRef arg_type;

	t = mini_get_underlying_type (t);
	size = get_vtype_size (t);

	if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type_internal (t)))
		address = LLVMBuildBitCast (ctx->builder, address, LLVMPointerType (LLVMInt8Type (), 0), "");

	if (ainfo->storage == LLVMArgAsFpArgs)
		nslots = ainfo->nslots;
	else
		nslots = 2;
	for (j = 0; j < nslots; ++j) {
		LLVMValueRef index [2], addr, daddr;
		int partsize = size > TARGET_SIZEOF_VOID_P ? TARGET_SIZEOF_VOID_P : size;

		if (ainfo->pair_storage [j] == LLVMArgNone)
			continue;

		switch (ainfo->pair_storage [j]) {
		case LLVMArgInIReg:
			if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type_internal (t))) {
				index [0] = LLVMConstInt (LLVMInt32Type (), j * TARGET_SIZEOF_VOID_P, FALSE);
				addr = LLVMBuildGEP2 (builder, LLVMInt8Type (), address, index, 1, "");
			} else {
				daddr = LLVMBuildBitCast (ctx->builder, address, LLVMPointerType (IntPtrType (), 0), "");
				index [0] = LLVMConstInt (LLVMInt32Type (), j, FALSE);
				addr = LLVMBuildGEP2 (builder, IntPtrType (), daddr, index, 1, "");
			}
			args [pindex ++] = convert (ctx, LLVMBuildLoad2 (builder, LLVMIntType (partsize * 8), LLVMBuildBitCast (ctx->builder, addr, LLVMPointerType (LLVMIntType (partsize * 8), 0), ""), ""), IntPtrType ());
			break;
		case LLVMArgInFPReg:
			if (ainfo->esize == 8)
				arg_type = LLVMDoubleType ();
			else
				arg_type = LLVMFloatType ();
			daddr = LLVMBuildBitCast (ctx->builder, address, LLVMPointerType (arg_type, 0), "");
			index [0] = LLVMConstInt (LLVMInt32Type (), j, FALSE);
			addr = LLVMBuildGEP2 (builder, arg_type, daddr, index, 1, "");
			args [pindex ++] = LLVMBuildLoad2 (builder, arg_type, addr, "");
			break;
		case LLVMArgNone:
			break;
		default:
			g_assert_not_reached ();
		}
		size -= TARGET_SIZEOF_VOID_P;
	}

	*nargs = pindex;
}

LLVMValueRef
build_alloca_llvm_type_name (EmitContext *ctx, LLVMTypeRef t, int align, const char *name)
{
	/*
	 * Have to place all alloca's at the end of the entry bb, since otherwise they would
	 * get executed every time control reaches them.
	 */
	LLVMPositionBuilder (ctx->alloca_builder, get_bb (ctx, ctx->cfg->bb_entry), ctx->last_alloca);

	ctx->last_alloca = mono_llvm_build_alloca (ctx->alloca_builder, t, NULL, align, name);
	return ctx->last_alloca;
}

#ifdef TARGET_ARM
/* Only used by the imt/rgctx stack-slot workaround in process_call () */
LLVMValueRef
build_alloca_llvm_type (EmitContext *ctx, LLVMTypeRef t, int align)
{
	return build_alloca_llvm_type_name (ctx, t, align, "");
}
#endif


LLVMValueRef
build_named_alloca (EmitContext *ctx, MonoType *t, char const *name)
{
	MonoClass *k = mono_class_from_mono_type_internal (t);
	int align;

	g_assert (!mini_is_gsharedvt_variable_type (t));

	if (MONO_CLASS_IS_SIMD (ctx->cfg, k))
		align = mono_class_value_size (k, NULL);
	else
		align = mono_class_min_align (k);

	/* Sometimes align is not a power of 2 */
	while (mono_is_power_of_two (align) == -1)
		align ++;

	return build_alloca_llvm_type_name (ctx, type_to_llvm_type (ctx, t), align, name);
}

/*
 * Pair a pointer with the element type it points at. Allocated from the
 * compile-time mempool, so it lives exactly as long as the EmitContext.
 */
Address*
create_address (EmitContext *ctx, LLVMValueRef value, LLVMTypeRef type)
{
	Address *res = static_cast<Address *>(mono_mempool_alloc0 (ctx->mempool, sizeof (Address)));
	res->value = value;
	res->type = type;
	return res;
}

Address*
build_alloca_address (EmitContext *ctx, MonoType *t)
{
	return create_address (ctx, build_named_alloca (ctx, t, ""), type_to_llvm_type (ctx, t));
}

Address*
build_named_alloca_address (EmitContext *ctx, MonoType *t, const char *name)
{
	return create_address (ctx, build_named_alloca (ctx, t, name), type_to_llvm_type (ctx, t));
}

LLVMValueRef
emit_gsharedvt_ldaddr (EmitContext *ctx, int vreg)
{
	/*
	 * gsharedvt local.
	 * Compute the address of the local as gsharedvt_locals_var + gsharedvt_info_var->locals_offsets [idx].
	 */
	MonoCompile *cfg = ctx->cfg;
	LLVMBuilderRef builder = ctx->builder;
	LLVMValueRef offset, offset_var;
	LLVMValueRef info_var = ctx->values [cfg->gsharedvt_info_var->dreg];
	LLVMValueRef locals_var = ctx->values [cfg->gsharedvt_locals_var->dreg];
	LLVMValueRef ptr;
	char *name;

	g_assert (info_var);
	g_assert (locals_var);

	int idx = cfg->gsharedvt_vreg_to_idx [vreg] - 1;

	offset = LLVMConstInt (LLVMInt32Type (), MONO_STRUCT_OFFSET (MonoGSharedVtMethodRuntimeInfo, entries) + (idx * TARGET_SIZEOF_VOID_P), FALSE);
	ptr = LLVMBuildAdd (builder, convert (ctx, info_var, IntPtrType ()), convert (ctx, offset, IntPtrType ()), "");

	name = g_strdup_printf ("gsharedvt_local_%d_offset", vreg);
	offset_var = LLVMBuildLoad2 (builder, LLVMInt32Type (), convert (ctx, ptr, LLVMPointerType (LLVMInt32Type (), 0)), name);

	return LLVMBuildAdd (builder, convert (ctx, locals_var, IntPtrType ()), convert (ctx, offset_var, IntPtrType ()), "");
}

/* Emit a wrapper around the parameterless JIT icall ICALL_ID with a cold calling convention */
LLVMValueRef
emit_icall_cold_wrapper (MonoLLVMModule *module, LLVMModuleRef lmodule, MonoJitICallId icall_id, gboolean aot)
{
	LLVMValueRef func, callee;
	LLVMBasicBlockRef entry_bb;
	LLVMBuilderRef builder;
	LLVMTypeRef sig;
	char *name;

	name = g_strdup_printf ("%s_icall_cold_wrapper_%d", module->global_prefix, icall_id);

	func = LLVMAddFunction (lmodule, name, LLVMFunctionType (LLVMVoidType (), NULL, 0, FALSE));
	sig = LLVMFunctionType (LLVMVoidType (), NULL, 0, FALSE);
	LLVMSetLinkage (func, LLVMInternalLinkage);
	mono_llvm_add_func_attr (func, LLVM_ATTR_NO_INLINE);
	set_cold_cconv (func);

	entry_bb = LLVMAppendBasicBlock (func, "ENTRY");
	builder = LLVMCreateBuilder ();
	LLVMPositionBuilderAtEnd (builder, entry_bb);

	if (aot) {
		callee = get_aotconst_module (module, builder, MONO_PATCH_INFO_JIT_ICALL_ID, GUINT_TO_POINTER (icall_id), LLVMPointerType (sig, 0), NULL, NULL);
	} else {
		MonoJitICallInfo * const info = mono_find_jit_icall_info (icall_id);
		gpointer target = const_cast<gpointer>(mono_icall_get_wrapper_full (info, TRUE));

		LLVMValueRef tramp_var = LLVMAddGlobal (lmodule, LLVMPointerType (sig, 0), name);
		LLVMSetInitializer (tramp_var, LLVMConstIntToPtr (LLVMConstInt (LLVMInt64Type (), static_cast<guint64>(reinterpret_cast<size_t>(target)), FALSE), LLVMPointerType (sig, 0)));
		LLVMSetLinkage (tramp_var, LLVMExternalLinkage);
		callee = LLVMBuildLoad2 (builder, LLVMPointerType (sig, 0), tramp_var, "");
	}
	LLVMBuildCall2 (builder, sig, callee, NULL, 0, "");

	LLVMBuildRetVoid (builder);

	LLVMVerifyFunction(func, LLVMAbortProcessAction);
	LLVMDisposeBuilder (builder);
	return func;
}

void
emit_gc_safepoint_poll (MonoLLVMModule *module, LLVMModuleRef lmodule, MonoCompile *cfg)
{
	bool is_aot = cfg == nullptr || cfg->compile_aot;
	LLVMValueRef func = mono_llvm_get_or_insert_gc_safepoint_poll (lmodule);
	mono_llvm_add_func_attr (func, LLVM_ATTR_NO_UNWIND);
	if (is_aot) {
#if TARGET_WIN32
		if (module->static_link)
			LLVMSetLinkage (func, LLVMInternalLinkage);
		else
#endif
			LLVMSetLinkage (func, LLVMWeakODRLinkage);
	} else {
		mono_llvm_add_func_attr (func, LLVM_ATTR_OPTIMIZE_NONE); // no need to waste time here, the function is already optimized and will be inlined.
		mono_llvm_add_func_attr (func, LLVM_ATTR_NO_INLINE); // optnone attribute requires noinline (but it will be inlined anyway)
		if (!module->gc_poll_cold_wrapper_compiled) {
			ERROR_DECL (error);
			/* Compiling a method here is a bit ugly, but it works */
			MonoMethod *wrapper = mono_marshal_get_llvm_func_wrapper (LLVM_FUNC_WRAPPER_GC_POLL);
			module->gc_poll_cold_wrapper_compiled = mono_jit_compile_method (wrapper, error);
			mono_error_assert_ok (error);
		}
	}
	LLVMBasicBlockRef entry_bb = LLVMAppendBasicBlock (func, "gc.safepoint_poll.entry");
	LLVMBasicBlockRef poll_bb = LLVMAppendBasicBlock (func, "gc.safepoint_poll.poll");
	LLVMBasicBlockRef exit_bb = LLVMAppendBasicBlock (func, "gc.safepoint_poll.exit");
	LLVMTypeRef ptr_type = LLVMPointerType (IntPtrType (), 0);
	LLVMBuilderRef builder = LLVMCreateBuilder ();

	/* entry: */
	LLVMPositionBuilderAtEnd (builder, entry_bb);
	LLVMValueRef poll_val_ptr;
	if (is_aot) {
		poll_val_ptr = get_aotconst_module (module, builder, MONO_PATCH_INFO_GC_SAFE_POINT_FLAG, NULL, ptr_type, NULL, NULL);
	} else {
		LLVMValueRef poll_val_int = LLVMConstInt (IntPtrType (), reinterpret_cast<guint64>(&mono_polling_required), FALSE);
		poll_val_ptr = LLVMBuildIntToPtr (builder, poll_val_int, ptr_type, "");
	}
	LLVMValueRef poll_val_ptr_load = LLVMBuildLoad2 (builder, IntPtrType (), poll_val_ptr, ""); // probably needs to be volatile
	LLVMValueRef poll_val = LLVMBuildPtrToInt (builder, poll_val_ptr_load, IntPtrType (), "");
	LLVMValueRef poll_val_zero = LLVMConstNull (LLVMTypeOf (poll_val));
	LLVMValueRef cmp = LLVMBuildICmp (builder, LLVMIntEQ, poll_val, poll_val_zero, "");
	mono_llvm_build_weighted_branch (builder, cmp, exit_bb, poll_bb, 1000 /* weight for exit_bb */, 1 /* weight for poll_bb */);

	/* poll: */
	LLVMPositionBuilderAtEnd (builder, poll_bb);
	LLVMValueRef call;
	if (is_aot) {
		LLVMValueRef icall_wrapper = emit_icall_cold_wrapper (module, lmodule, MONO_JIT_ICALL_mono_threads_state_poll, TRUE);
		module->gc_poll_cold_wrapper = icall_wrapper;
		call = LLVMBuildCall2 (builder, LLVMGlobalGetValueType (icall_wrapper), icall_wrapper, NULL, 0, "");
	} else {
		// in JIT mode we have to emit @gc.safepoint_poll function for each method (module)
		// this function calls gc_poll_cold_wrapper_compiled via a global variable.
		// @gc.safepoint_poll will be inlined and can be deleted after -place-safepoints pass.
		LLVMTypeRef poll_sig = LLVMFunctionType0 (LLVMVoidType (), FALSE);
		LLVMTypeRef poll_sig_ptr = LLVMPointerType (poll_sig, 0);
		gpointer target = resolve_patch (cfg, MONO_PATCH_INFO_ABS, module->gc_poll_cold_wrapper_compiled);
		LLVMValueRef tramp_var = LLVMAddGlobal (lmodule, poll_sig_ptr, "mono_threads_state_poll");
		LLVMValueRef target_val = LLVMConstInt (LLVMInt64Type (), reinterpret_cast<guint64>(target), FALSE);
		LLVMSetInitializer (tramp_var, LLVMConstIntToPtr (target_val, poll_sig_ptr));
		LLVMSetLinkage (tramp_var, LLVMExternalLinkage);
		LLVMValueRef callee = LLVMBuildLoad2 (builder, poll_sig_ptr, tramp_var, "");
		call = LLVMBuildCall2 (builder, poll_sig, callee, NULL, 0, "");
	}
	set_call_cold_cconv (call);
	LLVMBuildBr (builder, exit_bb);

	/* exit: */
	LLVMPositionBuilderAtEnd (builder, exit_bb);
	LLVMBuildRetVoid (builder);
	LLVMDisposeBuilder (builder);
}


#endif /* DISABLE_JIT */
