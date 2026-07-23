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
llvm::Value *
convert_full (EmitContext *ctx, llvm::Value *v, llvm::Type *dtype, gboolean is_unsigned)
{
	llvm::Type *stype = v->getType ();

	if (stype != dtype) {
		bool ext = false;

		/* Extend */
		if (dtype == llvm::Type::getInt64Ty (ctx->llvm_ctx ()) && (stype == llvm::Type::getInt32Ty (ctx->llvm_ctx ()) || stype == llvm::Type::getInt16Ty (ctx->llvm_ctx ()) || stype == llvm::Type::getInt8Ty (ctx->llvm_ctx ())))
			ext = true;
		else if (dtype == llvm::Type::getInt32Ty (ctx->llvm_ctx ()) && (stype == llvm::Type::getInt16Ty (ctx->llvm_ctx ()) || stype == llvm::Type::getInt8Ty (ctx->llvm_ctx ())))
			ext = true;
		else if (dtype == llvm::Type::getInt16Ty (ctx->llvm_ctx ()) && (stype == llvm::Type::getInt8Ty (ctx->llvm_ctx ())))
			ext = true;

		if (ext)
			return is_unsigned ? ctx->builder->CreateZExt (v, dtype, "") : ctx->builder->CreateSExt (v, dtype, "");

		if (dtype == llvm::Type::getDoubleTy (ctx->llvm_ctx ()) && stype == llvm::Type::getFloatTy (ctx->llvm_ctx ()))
			return ctx->builder->CreateFPExt (v, dtype, "");

		/* Trunc */
		if (stype == llvm::Type::getInt64Ty (ctx->llvm_ctx ()) && (dtype == llvm::Type::getInt32Ty (ctx->llvm_ctx ()) || dtype == llvm::Type::getInt16Ty (ctx->llvm_ctx ()) || dtype == llvm::Type::getInt8Ty (ctx->llvm_ctx ())))
			return ctx->builder->CreateTrunc (v, dtype, "");
		if (stype == llvm::Type::getInt32Ty (ctx->llvm_ctx ()) && (dtype == llvm::Type::getInt16Ty (ctx->llvm_ctx ()) || dtype == llvm::Type::getInt8Ty (ctx->llvm_ctx ())))
			return ctx->builder->CreateTrunc (v, dtype, "");
		if (stype == llvm::Type::getInt16Ty (ctx->llvm_ctx ()) && dtype == llvm::Type::getInt8Ty (ctx->llvm_ctx ()))
			return ctx->builder->CreateTrunc (v, dtype, "");
		if (stype == llvm::Type::getDoubleTy (ctx->llvm_ctx ()) && dtype == llvm::Type::getFloatTy (ctx->llvm_ctx ()))
			return ctx->builder->CreateFPTrunc (v, dtype, "");

		if (stype->isPointerTy () && dtype->isPointerTy ())
			return ctx->builder->CreateBitCast (v, dtype, "");
		if (dtype->isPointerTy ())
			return ctx->builder->CreateIntToPtr (v, dtype, "");
		if (stype->isPointerTy ())
			return ctx->builder->CreatePtrToInt (v, dtype, "");

		if (mono_arch_is_soft_float ()) {
			if (stype == llvm::Type::getInt32Ty (ctx->llvm_ctx ()) && dtype == llvm::Type::getFloatTy (ctx->llvm_ctx ()))
				return ctx->builder->CreateBitCast (v, dtype, "");
			if (stype == llvm::Type::getInt32Ty (ctx->llvm_ctx ()) && dtype == llvm::Type::getDoubleTy (ctx->llvm_ctx ()))
				return ctx->builder->CreateBitCast (ctx->builder->CreateZExt (v, llvm::Type::getInt64Ty (ctx->llvm_ctx ()), ""), dtype, "");
		}

		if (stype->getTypeID () == llvm::Type::FixedVectorTyID && dtype->getTypeID () == llvm::Type::FixedVectorTyID)
			return ctx->builder->CreateBitCast (v, dtype, "");

		mono_llvm_dump_value (llvm::wrap (v));
		mono_llvm_dump_type (llvm::wrap (dtype));
		printf ("\n");
		g_assert_not_reached ();
		return nullptr;
	} else {
		return v;
	}
}

llvm::Value *
convert (EmitContext *ctx, llvm::Value *v, llvm::Type *dtype)
{
	return convert_full (ctx, v, dtype, FALSE);
}

void
emit_memset (EmitContext *ctx, llvm::IRBuilder<> *builder, LLVMValueRef v, LLVMValueRef size, int alignment)
{
	LLVMValueRef args [5];
	int aindex = 0;

	args [aindex ++] = v;
	args [aindex ++] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt8Ty (ctx->llvm_ctx ()), 0, false));
	args [aindex ++] = size;
#if LLVM_API_VERSION < 900
	args [aindex ++] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), alignment, false));
#endif
	args [aindex ++] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt1Ty (ctx->llvm_ctx ()), 0, false));
	llvm::wrap (builder->CreateCall (llvm::cast<llvm::FunctionType> (llvm::unwrap (LLVMGlobalGetValueType (get_intrins (ctx, INTRINS_MEMSET)))), llvm::unwrap (get_intrins (ctx, INTRINS_MEMSET)), gep_index_list (args, aindex), ""));
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

	v = mono_llvm_build_load (llvm::wrap (ctx->builder), llvm::wrap (ctx->addresses [vreg]->type), llvm::wrap (ctx->addresses [vreg]->value), "", TRUE);
	t = ctx->vreg_cli_types [vreg];
	if (t && !t->byref) {
		/* 
		 * Might have to zero extend since llvm doesn't have 
		 * unsigned types.
		 */
		if (t->type == MONO_TYPE_U1 || t->type == MONO_TYPE_U2 || t->type == MONO_TYPE_CHAR || t->type == MONO_TYPE_BOOLEAN)
			v = llvm::wrap (ctx->builder->CreateZExt (llvm::unwrap (v), llvm::Type::getInt32Ty (ctx->llvm_ctx ()), ""));
		else if (t->type == MONO_TYPE_I1 || t->type == MONO_TYPE_I2)
			v = llvm::wrap (ctx->builder->CreateSExt (llvm::unwrap (v), llvm::Type::getInt32Ty (ctx->llvm_ctx ()), ""));
		else if (t->type == MONO_TYPE_U8)
			v = llvm::wrap (ctx->builder->CreateZExt (llvm::unwrap (v), llvm::Type::getInt64Ty (ctx->llvm_ctx ()), ""));
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
		llvm::wrap (ctx->builder->CreateStore (convert (ctx, ctx->values [vreg], llvm::unwrap (type_to_llvm_type (ctx, var->inst_vtype))), ctx->addresses [vreg]->value));
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
			ret_type = llvm::wrap (llvm::Type::getVoidTy (ctx->llvm_ctx ()));
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
			ret_type = llvm::wrap (llvm::Type::getIntNTy (ctx->llvm_ctx (), size * 8));
		} else {
			g_assert (cinfo->ret.nslots == 1 || cinfo->ret.nslots == 2);
			ret_type = llvm::wrap (llvm::Type::getIntNTy (ctx->llvm_ctx (), cinfo->ret.nslots * sizeof (target_mgreg_t) * 8));
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
			members [i] = cinfo->ret.esize == 8 ? llvm::wrap (llvm::Type::getDoubleTy (ctx->llvm_ctx ())) : llvm::wrap (llvm::Type::getFloatTy (ctx->llvm_ctx ()));
		ret_type = LLVMStructType (members, cinfo->ret.nslots, FALSE);
		break;
	}
	case LLVMArgVtypeByRef:
		/* Vtype returned using a hidden argument */
		ret_type = llvm::wrap (llvm::Type::getVoidTy (ctx->llvm_ctx ()));
		break;
	case LLVMArgVtypeRetAddr:
	case LLVMArgGsharedvtFixed:
	case LLVMArgGsharedvtFixedVtype:
	case LLVMArgGsharedvtVariable:
		vretaddr = true;
		ret_type = llvm::wrap (llvm::Type::getVoidTy (ctx->llvm_ctx ()));
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
		param_types [pindex] = llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0));
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
					param_types [pindex ++] = llvm::wrap (llvm::Type::getIntNTy (ctx->llvm_ctx (), TARGET_SIZEOF_VOID_P * 8));
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
			param_types [pindex] = llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0));
			pindex ++;
			break;
		case LLVMArgAsIArgs:
			if (ainfo->esize == 8)
				param_types [pindex] = LLVMArrayType (llvm::wrap (llvm::Type::getInt64Ty (ctx->llvm_ctx ())), ainfo->nslots);
			else
				param_types [pindex] = LLVMArrayType (IntPtrType (), ainfo->nslots);
			pindex ++;
			break;
		case LLVMArgVtypeAddr:
		case LLVMArgVtypeByRef:
			param_types [pindex] = type_to_llvm_arg_type (ctx, ainfo->type);
			if (!ctx_ok (ctx))
				break;
			param_types [pindex] = llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0));
			pindex ++;
			break;
		case LLVMArgAsFpArgs: {
			int j;

			/* Emit dummy fp arguments if needed so the rest is passed on the stack */
			for (j = 0; j < ainfo->ndummy_fpargs; ++j)
				param_types [pindex ++] = llvm::wrap (llvm::Type::getDoubleTy (ctx->llvm_ctx ()));
			for (j = 0; j < ainfo->nslots; ++j)
				param_types [pindex ++] = ainfo->esize == 8 ? llvm::wrap (llvm::Type::getDoubleTy (ctx->llvm_ctx ())) : llvm::wrap (llvm::Type::getFloatTy (ctx->llvm_ctx ()));
			break;
		}
		case LLVMArgVtypeAsScalar:
			g_assert_not_reached ();
			break;
		case LLVMArgGsharedvtFixed:
		case LLVMArgGsharedvtFixedVtype:
			param_types [pindex ++] = llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0));
			break;
		case LLVMArgGsharedvtVariable:
			param_types [pindex ++] = llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0));
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
llvm::IRBuilder<> *
create_builder (EmitContext *ctx)
{
	auto *b = new llvm::IRBuilder<> (llvm_global_ctx ());
	if (mono_use_fast_math)
		mono_llvm_set_fast_math (llvm::wrap (b));

	ctx->builders.emplace_back (b);

	return b;
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
get_aotconst_module (MonoLLVMModule *module, llvm::IRBuilder<> *builder, MonoJumpInfoType type, gconstpointer data, LLVMTypeRef llvm_type,
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
		LLVMSetInitializer (v, llvm::wrap (llvm::Constant::getNullValue (llvm::unwrap (type))));
		// FIXME:
		LLVMSetAlignment (v, 8);

		g_hash_table_insert (module->aotconst_vars, GINT_TO_POINTER (got_offset), v);
		const_var = v;
	}

	load = llvm::wrap (builder->CreateLoad (llvm::unwrap (llvm_type), llvm::unwrap (const_var), ""));

	if (mono_aot_is_shared_got_offset (got_offset))
		set_invariant_load_flag (load);
	if (type == MONO_PATCH_INFO_LDSTR)
		set_nonnull_load_flag (load);

	load = llvm::wrap (builder->CreateBitCast (llvm::unwrap (load), llvm::unwrap (llvm_type), ""));

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

	LLVMValueRef tramp_var = LLVMAddGlobal (ctx->lmodule, llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)), name);
	LLVMSetInitializer (tramp_var, llvm::wrap (llvm::ConstantExpr::getIntToPtr (llvm::cast<llvm::Constant> (llvm::ConstantInt::get (llvm::Type::getInt64Ty (ctx->llvm_ctx ()), static_cast<guint64>(reinterpret_cast<size_t>(target)), false)), llvm::PointerType::get (ctx->llvm_ctx (), 0))));
	LLVMSetLinkage (tramp_var, LLVMExternalLinkage);
	LLVMValueRef callee = llvm::wrap (ctx->builder->CreateLoad (llvm::PointerType::get (ctx->llvm_ctx (), 0), llvm::unwrap (tramp_var), ""));
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
emit_call (EmitContext *ctx, MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref, LLVMTypeRef sig, LLVMValueRef callee, LLVMValueRef *args, int pindex)
{
	MonoCompile *cfg = ctx->cfg;
	LLVMValueRef lcall = nullptr;
	llvm::IRBuilder<> *builder = *builder_ref;
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
			lcall = llvm::wrap (builder->CreateInvoke (llvm::cast<llvm::FunctionType> (llvm::unwrap (sig)), llvm::unwrap (callee), llvm::unwrap (noex_bb), llvm::unwrap (ex_bb), gep_index_list (args, pindex), ""));

			builder = ctx->builder = create_builder (ctx);
			ctx->builder->SetInsertPoint (llvm::unwrap (noex_bb));

			ctx->bblocks [bb->block_num].end_bblock = noex_bb;
		}
	}
	
	if (!lcall) {
		lcall = llvm::wrap (builder->CreateCall (llvm::cast<llvm::FunctionType> (llvm::unwrap (sig)), llvm::unwrap (callee), gep_index_list (args, pindex), ""));
		ctx->builder = builder;
	}

	if (builder_ref)
		*builder_ref = ctx->builder;

	return lcall;
}

LLVMValueRef
emit_load (EmitContext *ctx, MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref, int size, LLVMTypeRef type, LLVMValueRef addr, LLVMValueRef base, const char *name, gboolean is_faulting, gboolean is_volatile, BarrierKind barrier)
{
	LLVMValueRef res;

	/* 
	 * We emit volatile loads for loads which can fault, because otherwise
	 * LLVM will generate invalid code when encountering a load from a
	 * NULL address.
	 */
	if (barrier != LLVM_BARRIER_NONE)
		res = mono_llvm_build_atomic_load (llvm::wrap (*builder_ref), type, addr, name, is_volatile, size, barrier);
	else
		res = mono_llvm_build_load (llvm::wrap (*builder_ref), type, addr, name, is_volatile);

	return res;
}

void
emit_store_general (EmitContext *ctx, MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref, int size, LLVMValueRef value, LLVMValueRef addr, LLVMValueRef base, gboolean is_faulting, gboolean is_volatile, BarrierKind barrier)
{
	if (barrier != LLVM_BARRIER_NONE)
		mono_llvm_build_aligned_store (llvm::wrap (*builder_ref), value, addr, barrier, size);
	else
		mono_llvm_build_store (llvm::wrap (*builder_ref), value, addr, is_volatile, barrier);
}

void
emit_store (EmitContext *ctx, MonoBasicBlock *bb, llvm::IRBuilder<> **builder_ref, int size, LLVMValueRef value, LLVMValueRef addr, LLVMValueRef base, gboolean is_faulting, gboolean is_volatile)
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
	llvm::IRBuilder<> *builder;
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

	LLVMValueRef branch = llvm::wrap (ctx->builder->CreateCondBr (llvm::unwrap (cmp), llvm::unwrap (ex_bb), llvm::unwrap (noex_bb)));
	if (exc_id == MONO_EXC_NULL_REF && !ctx->cfg->disable_llvm_implicit_null_checks && !force_explicit) {
		mono_llvm_set_implicit_branch (llvm::wrap (ctx->builder), branch);
	}

	/* Emit exception throwing code */
	ctx->builder = builder = create_builder (ctx);
	builder->SetInsertPoint (llvm::unwrap (ex_bb));

	callee = ctx->module->throw_corlib_exception;

	/*
	 * Hoisted out of the !callee branch: emit_call () needs the signature the
	 * callee was declared with, and the callee may come from the module cache.
	 * Never derive it from the callee value itself.
	 */
	LLVMTypeRef sig;
	if (no_pc)
		sig = LLVMFunctionType1 (llvm::wrap (llvm::Type::getVoidTy (ctx->llvm_ctx ())), llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ())), FALSE);
	else
		sig = LLVMFunctionType2 (llvm::wrap (llvm::Type::getVoidTy (ctx->llvm_ctx ())), llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ())), llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0)), FALSE);

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
			llvm::wrap (builder->CreateBr (llvm::unwrap (ex2_bb)));
			ex_bb = ex2_bb;

			ctx->builder = builder = create_builder (ctx);
			ctx->builder->SetInsertPoint (llvm::unwrap (ex2_bb));
		}
	}

	args [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), m_class_get_type_token (exc_class) - MONO_TOKEN_TYPE_DEF, false));

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

	llvm::wrap (builder->CreateUnreachable ());

	ctx->builder = builder = create_builder (ctx);
	ctx->builder->SetInsertPoint (llvm::unwrap (noex_bb));

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
emit_args_to_vtype (EmitContext *ctx, llvm::IRBuilder<> *builder, MonoType *t, LLVMValueRef address, LLVMArgInfo *ainfo, LLVMValueRef *args)
{
	int j, size, nslots;
	MonoClass *klass;

	t = mini_get_underlying_type (t);
	klass = mono_class_from_mono_type_internal (t);
	size = mono_class_value_size (klass, NULL);

	if (MONO_CLASS_IS_SIMD (ctx->cfg, klass))
		address = llvm::wrap (ctx->builder->CreateBitCast (llvm::unwrap (address), llvm::PointerType::get (ctx->llvm_ctx (), 0), ""));

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
			part_type = llvm::wrap (llvm::Type::getIntNTy (ctx->llvm_ctx (), part_size * 8));
			if (MONO_CLASS_IS_SIMD (ctx->cfg, klass)) {
				index [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), j * TARGET_SIZEOF_VOID_P, false));
				addr = llvm::wrap (builder->CreateGEP (llvm::Type::getInt8Ty (ctx->llvm_ctx ()), llvm::unwrap (address), gep_index_list (index, 1), ""));
			} else {
				daddr = llvm::wrap (ctx->builder->CreateBitCast (llvm::unwrap (address), llvm::PointerType::get (ctx->llvm_ctx (), 0), ""));
				index [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), j, false));
				addr = llvm::wrap (builder->CreateGEP (llvm::unwrap (IntPtrType ()), llvm::unwrap (daddr), gep_index_list (index, 1), ""));
			}
			llvm::wrap (builder->CreateStore (convert (ctx, llvm::unwrap (args [j]), llvm::unwrap (part_type)), ctx->builder->CreateBitCast (llvm::unwrap (addr), llvm::PointerType::get (ctx->llvm_ctx (), 0), "")));
			break;
		}
		case LLVMArgInFPReg: {
			LLVMTypeRef arg_type;

			if (ainfo->esize == 8)
				arg_type = llvm::wrap (llvm::Type::getDoubleTy (ctx->llvm_ctx ()));
			else
				arg_type = llvm::wrap (llvm::Type::getFloatTy (ctx->llvm_ctx ()));

			index [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), j, false));
			daddr = llvm::wrap (ctx->builder->CreateBitCast (llvm::unwrap (address), llvm::PointerType::get (ctx->llvm_ctx (), 0), ""));
			addr = llvm::wrap (builder->CreateGEP (llvm::unwrap (arg_type), llvm::unwrap (daddr), gep_index_list (index, 1), ""));
			llvm::wrap (builder->CreateStore (llvm::unwrap (args [j]), llvm::unwrap (addr)));
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
emit_vtype_to_args (EmitContext *ctx, llvm::IRBuilder<> *builder, MonoType *t, LLVMValueRef address, LLVMArgInfo *ainfo, LLVMValueRef *args, guint32 *nargs)
{
	int pindex = 0;
	int j, size, nslots;
	LLVMTypeRef arg_type;

	t = mini_get_underlying_type (t);
	size = get_vtype_size (t);

	if (MONO_CLASS_IS_SIMD (ctx->cfg, mono_class_from_mono_type_internal (t)))
		address = llvm::wrap (ctx->builder->CreateBitCast (llvm::unwrap (address), llvm::PointerType::get (ctx->llvm_ctx (), 0), ""));

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
				index [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), j * TARGET_SIZEOF_VOID_P, false));
				addr = llvm::wrap (builder->CreateGEP (llvm::Type::getInt8Ty (ctx->llvm_ctx ()), llvm::unwrap (address), gep_index_list (index, 1), ""));
			} else {
				daddr = llvm::wrap (ctx->builder->CreateBitCast (llvm::unwrap (address), llvm::PointerType::get (ctx->llvm_ctx (), 0), ""));
				index [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), j, false));
				addr = llvm::wrap (builder->CreateGEP (llvm::unwrap (IntPtrType ()), llvm::unwrap (daddr), gep_index_list (index, 1), ""));
			}
			args [pindex ++] = llvm::wrap (convert (ctx, builder->CreateLoad (llvm::Type::getIntNTy (ctx->llvm_ctx (), partsize * 8), ctx->builder->CreateBitCast (llvm::unwrap (addr), llvm::PointerType::get (ctx->llvm_ctx (), 0), ""), ""), llvm::unwrap (IntPtrType ())));
			break;
		case LLVMArgInFPReg:
			if (ainfo->esize == 8)
				arg_type = llvm::wrap (llvm::Type::getDoubleTy (ctx->llvm_ctx ()));
			else
				arg_type = llvm::wrap (llvm::Type::getFloatTy (ctx->llvm_ctx ()));
			daddr = llvm::wrap (ctx->builder->CreateBitCast (llvm::unwrap (address), llvm::PointerType::get (ctx->llvm_ctx (), 0), ""));
			index [0] = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), j, false));
			addr = llvm::wrap (builder->CreateGEP (llvm::unwrap (arg_type), llvm::unwrap (daddr), gep_index_list (index, 1), ""));
			args [pindex ++] = llvm::wrap (builder->CreateLoad (llvm::unwrap (arg_type), llvm::unwrap (addr), ""));
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
	LLVMPositionBuilder (llvm::wrap (ctx->alloca_builder), get_bb (ctx, ctx->cfg->bb_entry), llvm::wrap (ctx->last_alloca));

	LLVMValueRef alloca = mono_llvm_build_alloca (llvm::wrap (ctx->alloca_builder), t, NULL, align, name);
	ctx->last_alloca = llvm::unwrap (alloca);
	return alloca;
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
	res->value = llvm::unwrap (value);
	res->type = llvm::unwrap (type);
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
	llvm::IRBuilder<> *builder = ctx->builder;
	LLVMValueRef offset, offset_var;
	LLVMValueRef info_var = llvm::wrap (ctx->values [cfg->gsharedvt_info_var->dreg]);
	LLVMValueRef locals_var = llvm::wrap (ctx->values [cfg->gsharedvt_locals_var->dreg]);
	LLVMValueRef ptr;
	char *name;

	g_assert (info_var);
	g_assert (locals_var);

	int idx = cfg->gsharedvt_vreg_to_idx [vreg] - 1;

	offset = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), MONO_STRUCT_OFFSET (MonoGSharedVtMethodRuntimeInfo, entries) + (idx * TARGET_SIZEOF_VOID_P), false));
	ptr = llvm::wrap (builder->CreateAdd (convert (ctx, llvm::unwrap (info_var), llvm::unwrap (IntPtrType ())), convert (ctx, llvm::unwrap (offset), llvm::unwrap (IntPtrType ())), ""));

	name = g_strdup_printf ("gsharedvt_local_%d_offset", vreg);
	offset_var = llvm::wrap (builder->CreateLoad (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), convert (ctx, llvm::unwrap (ptr), llvm::PointerType::get (ctx->llvm_ctx (), 0)), name));

	return llvm::wrap (builder->CreateAdd (convert (ctx, llvm::unwrap (locals_var), llvm::unwrap (IntPtrType ())), convert (ctx, llvm::unwrap (offset_var), llvm::unwrap (IntPtrType ())), ""));
}

/* Emit a wrapper around the parameterless JIT icall ICALL_ID with a cold calling convention */
LLVMValueRef
emit_icall_cold_wrapper (MonoLLVMModule *module, LLVMModuleRef lmodule, MonoJitICallId icall_id, gboolean aot)
{
	LLVMValueRef func, callee;
	LLVMBasicBlockRef entry_bb;
	llvm::IRBuilder<> *builder;
	LLVMTypeRef sig;
	char *name;

	name = g_strdup_printf ("%s_icall_cold_wrapper_%d", module->global_prefix, icall_id);

	func = LLVMAddFunction (lmodule, name, LLVMFunctionType (llvm::wrap (llvm::Type::getVoidTy (llvm_global_ctx ())), NULL, 0, FALSE));
	sig = LLVMFunctionType (llvm::wrap (llvm::Type::getVoidTy (llvm_global_ctx ())), NULL, 0, FALSE);
	LLVMSetLinkage (func, LLVMInternalLinkage);
	mono_llvm_add_func_attr (func, LLVM_ATTR_NO_INLINE);
	set_cold_cconv (func);

	entry_bb = LLVMAppendBasicBlock (func, "ENTRY");
	builder = new llvm::IRBuilder<> (llvm_global_ctx ());
	builder->SetInsertPoint (llvm::unwrap (entry_bb));

	if (aot) {
		callee = get_aotconst_module (module, builder, MONO_PATCH_INFO_JIT_ICALL_ID, GUINT_TO_POINTER (icall_id), llvm::wrap (llvm::PointerType::get (llvm_global_ctx (), 0)), NULL, NULL);
	} else {
		MonoJitICallInfo * const info = mono_find_jit_icall_info (icall_id);
		gpointer target = const_cast<gpointer>(mono_icall_get_wrapper_full (info, TRUE));

		LLVMValueRef tramp_var = LLVMAddGlobal (lmodule, llvm::wrap (llvm::PointerType::get (llvm_global_ctx (), 0)), name);
		LLVMSetInitializer (tramp_var, llvm::wrap (llvm::ConstantExpr::getIntToPtr (llvm::cast<llvm::Constant> (llvm::ConstantInt::get (llvm::Type::getInt64Ty (llvm_global_ctx ()), static_cast<guint64>(reinterpret_cast<size_t>(target)), false)), llvm::PointerType::get (llvm_global_ctx (), 0))));
		LLVMSetLinkage (tramp_var, LLVMExternalLinkage);
		callee = llvm::wrap (builder->CreateLoad (llvm::PointerType::get (llvm_global_ctx (), 0), llvm::unwrap (tramp_var), ""));
	}
	llvm::wrap (builder->CreateCall (llvm::cast<llvm::FunctionType> (llvm::unwrap (sig)), llvm::unwrap (callee), gep_index_list (NULL, 0), ""));

	llvm::wrap (builder->CreateRetVoid ());

	LLVMVerifyFunction(func, LLVMAbortProcessAction);
	delete builder;
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
	LLVMTypeRef ptr_type = llvm::wrap (llvm::PointerType::get (llvm_global_ctx (), 0));
	llvm::IRBuilder<> *builder = new llvm::IRBuilder<> (llvm_global_ctx ());

	/* entry: */
	builder->SetInsertPoint (llvm::unwrap (entry_bb));
	LLVMValueRef poll_val_ptr;
	if (is_aot) {
		poll_val_ptr = get_aotconst_module (module, builder, MONO_PATCH_INFO_GC_SAFE_POINT_FLAG, NULL, ptr_type, NULL, NULL);
	} else {
		LLVMValueRef poll_val_int = llvm::wrap (llvm::ConstantInt::get (llvm::unwrap (IntPtrType ()), reinterpret_cast<guint64>(&mono_polling_required), false));
		poll_val_ptr = llvm::wrap (builder->CreateIntToPtr (llvm::unwrap (poll_val_int), llvm::unwrap (ptr_type), ""));
	}
	LLVMValueRef poll_val_ptr_load = llvm::wrap (builder->CreateLoad (llvm::unwrap (IntPtrType ()), llvm::unwrap (poll_val_ptr), "")); // probably needs to be volatile
	LLVMValueRef poll_val = llvm::wrap (builder->CreatePtrToInt (llvm::unwrap (poll_val_ptr_load), llvm::unwrap (IntPtrType ()), ""));
	LLVMValueRef poll_val_zero = llvm::wrap (llvm::Constant::getNullValue (llvm::unwrap (LLVMTypeOf (poll_val))));
	LLVMValueRef cmp = llvm::wrap (builder->CreateICmp (to_llvm_pred (LLVMIntEQ), llvm::unwrap (poll_val), llvm::unwrap (poll_val_zero), ""));
	mono_llvm_build_weighted_branch (llvm::wrap (builder), cmp, exit_bb, poll_bb, 1000 /* weight for exit_bb */, 1 /* weight for poll_bb */);

	/* poll: */
	builder->SetInsertPoint (llvm::unwrap (poll_bb));
	LLVMValueRef call;
	if (is_aot) {
		LLVMValueRef icall_wrapper = emit_icall_cold_wrapper (module, lmodule, MONO_JIT_ICALL_mono_threads_state_poll, TRUE);
		module->gc_poll_cold_wrapper = icall_wrapper;
		call = llvm::wrap (builder->CreateCall (llvm::cast<llvm::FunctionType> (llvm::unwrap (LLVMGlobalGetValueType (icall_wrapper))), llvm::unwrap (icall_wrapper), gep_index_list (NULL, 0), ""));
	} else {
		// in JIT mode we have to emit @gc.safepoint_poll function for each method (module)
		// this function calls gc_poll_cold_wrapper_compiled via a global variable.
		// @gc.safepoint_poll will be inlined and can be deleted after -place-safepoints pass.
		LLVMTypeRef poll_sig = LLVMFunctionType0 (llvm::wrap (llvm::Type::getVoidTy (llvm_global_ctx ())), FALSE);
		LLVMTypeRef poll_sig_ptr = llvm::wrap (llvm::PointerType::get (llvm_global_ctx (), 0));
		gpointer target = resolve_patch (cfg, MONO_PATCH_INFO_ABS, module->gc_poll_cold_wrapper_compiled);
		LLVMValueRef tramp_var = LLVMAddGlobal (lmodule, poll_sig_ptr, "mono_threads_state_poll");
		LLVMValueRef target_val = llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt64Ty (llvm_global_ctx ()), reinterpret_cast<guint64>(target), false));
		LLVMSetInitializer (tramp_var, llvm::wrap (llvm::ConstantExpr::getIntToPtr (llvm::cast<llvm::Constant> (llvm::unwrap (target_val)), llvm::unwrap (poll_sig_ptr))));
		LLVMSetLinkage (tramp_var, LLVMExternalLinkage);
		LLVMValueRef callee = llvm::wrap (builder->CreateLoad (llvm::unwrap (poll_sig_ptr), llvm::unwrap (tramp_var), ""));
		call = llvm::wrap (builder->CreateCall (llvm::cast<llvm::FunctionType> (llvm::unwrap (poll_sig)), llvm::unwrap (callee), gep_index_list (NULL, 0), ""));
	}
	set_call_cold_cconv (call);
	llvm::wrap (builder->CreateBr (llvm::unwrap (exit_bb)));

	/* exit: */
	builder->SetInsertPoint (llvm::unwrap (exit_bb));
	llvm::wrap (builder->CreateRetVoid ());
	delete builder;
}


#endif /* DISABLE_JIT */
