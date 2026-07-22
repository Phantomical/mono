/**
 * \file
 * translator-intrinsics.cpp: the llvm.* intrinsic declaration cache.
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

static LLVMValueRef get_intrins_from_module (LLVMModuleRef lmodule, int id);

static inline void
add_func (LLVMModuleRef module, const char *name, LLVMTypeRef ret_type, LLVMTypeRef *param_types, int nparams)
{
	LLVMAddFunction (module, name, LLVMFunctionType (ret_type, param_types, nparams, FALSE));
}

static LLVMValueRef
add_intrins1 (LLVMModuleRef module, IntrinsicId id, LLVMTypeRef param1)
{
	return mono_llvm_register_overloaded_intrinsic (module, id, &param1, 1);
}

static LLVMValueRef
add_intrins2 (LLVMModuleRef module, IntrinsicId id, LLVMTypeRef param1, LLVMTypeRef param2)
{
	LLVMTypeRef params [] = { param1, param2 };
	return mono_llvm_register_overloaded_intrinsic (module, id, params, 2);
}

static LLVMValueRef
add_intrins3 (LLVMModuleRef module, IntrinsicId id, LLVMTypeRef param1, LLVMTypeRef param2, LLVMTypeRef param3)
{
	LLVMTypeRef params [] = { param1, param2, param3 };
	return mono_llvm_register_overloaded_intrinsic (module, id, params, 3);
}

static void
add_intrinsic (LLVMModuleRef module, IntrinsicId id)
{
	/* Register simple intrinsics */
	LLVMValueRef intrins = mono_llvm_register_intrinsic (module, static_cast<IntrinsicId>(id));
	if (intrins) {
		g_hash_table_insert (intrins_id_to_intrins, GINT_TO_POINTER (id), intrins);
		return;
	}

	/* Register overloaded intrinsics */
	switch (id) {
	case INTRINS_PREFETCH:
		/* llvm.prefetch.p0 in LLVM 18: overloaded on the pointer type. */
		intrins = add_intrins1 (module, id, LLVMPointerType (LLVMInt8Type (), 0));
		break;
	case INTRINS_MEMSET:
		intrins = add_intrins2 (module, id, LLVMPointerType (LLVMInt8Type (), 0), LLVMInt32Type ());
		break;
	case INTRINS_MEMCPY:
		intrins = add_intrins3 (module, id, LLVMPointerType (LLVMInt8Type (), 0), LLVMPointerType (LLVMInt8Type (), 0), LLVMInt32Type ());
		break;
	case INTRINS_MEMMOVE:
		intrins = add_intrins3 (module, id, LLVMPointerType (LLVMInt8Type (), 0), LLVMPointerType (LLVMInt8Type (), 0), LLVMInt64Type ());
		break;
	case INTRINS_SADD_OVF_I32:
	case INTRINS_UADD_OVF_I32:
	case INTRINS_SSUB_OVF_I32:
	case INTRINS_USUB_OVF_I32:
	case INTRINS_SMUL_OVF_I32:
	case INTRINS_UMUL_OVF_I32:
		intrins = add_intrins1 (module, id, LLVMInt32Type ());
		break;
	case INTRINS_SADD_OVF_I64:
	case INTRINS_UADD_OVF_I64:
	case INTRINS_SSUB_OVF_I64:
	case INTRINS_USUB_OVF_I64:
	case INTRINS_SMUL_OVF_I64:
	case INTRINS_UMUL_OVF_I64:
		intrins = add_intrins1 (module, id, LLVMInt64Type ());
		break;
	case INTRINS_FMA:
	case INTRINS_EXP:
	case INTRINS_LOG:
	case INTRINS_LOG2:
	case INTRINS_LOG10:
	case INTRINS_TRUNC:
	case INTRINS_SIN:
	case INTRINS_COS:
	case INTRINS_SQRT:
	case INTRINS_FLOOR:
	case INTRINS_CEIL:
	case INTRINS_FABS:
	case INTRINS_COPYSIGN:
	case INTRINS_POW:
		intrins = add_intrins1 (module, id, LLVMDoubleType ());
		break;
	case INTRINS_FMAF:
	case INTRINS_EXPF:
	case INTRINS_LOG2F:
	case INTRINS_LOG10F:
	case INTRINS_TRUNCF:
	case INTRINS_SINF:
	case INTRINS_COSF:
	case INTRINS_SQRTF:
	case INTRINS_FLOORF:
	case INTRINS_CEILF:
	case INTRINS_ABSF:
	case INTRINS_COPYSIGNF:
	case INTRINS_POWF:
		intrins = add_intrins1 (module, id, LLVMFloatType ());
		break;
	case INTRINS_EXPECT_I8:
		intrins = add_intrins1 (module, id, LLVMInt8Type ());
		break;
	case INTRINS_EXPECT_I1:
		intrins = add_intrins1 (module, id, LLVMInt1Type ());
		break;
	case INTRINS_CTPOP_I32:
	case INTRINS_CTLZ_I32:
	case INTRINS_CTTZ_I32:
	case INTRINS_BEXTR_I32:
	case INTRINS_BZHI_I32:
	case INTRINS_PEXT_I32:
	case INTRINS_PDEP_I32:
		intrins = add_intrins1 (module, id, LLVMInt32Type ());
		break;
	case INTRINS_CTPOP_I64:
	case INTRINS_BEXTR_I64:
	case INTRINS_BZHI_I64:
	case INTRINS_PEXT_I64:
	case INTRINS_PDEP_I64:
	case INTRINS_CTLZ_I64:
	case INTRINS_CTTZ_I64:
		intrins = add_intrins1 (module, id, LLVMInt64Type ());
		break;
#if defined(TARGET_AMD64) || defined(TARGET_X86)
	case INTRINS_SSE_SADD_SATI8:
	case INTRINS_SSE_UADD_SATI8:
	case INTRINS_SSE_SSUB_SATI8:
	case INTRINS_SSE_USUB_SATI8:
		intrins = add_intrins1 (module, id, sse_i1_t);
		break;
	case INTRINS_SSE_SADD_SATI16:
	case INTRINS_SSE_UADD_SATI16:
	case INTRINS_SSE_SSUB_SATI16:
	case INTRINS_SSE_USUB_SATI16:
		intrins = add_intrins1 (module, id, sse_i2_t);
		break;
#if LLVM_API_VERSION >= 700
	case INTRINS_SSE_SQRT_PS:
		intrins = add_intrins1 (module, id, sse_r4_t);
		break;
	case INTRINS_SSE_SQRT_PD:
		intrins = add_intrins1 (module, id, sse_r8_t);
		break;
	case INTRINS_SSE_SQRT_SS:
		intrins = add_intrins1 (module, id, LLVMFloatType ());
		break;
	case INTRINS_SSE_SQRT_SD:
		intrins = add_intrins1 (module, id, LLVMDoubleType ());
		break;
#endif /* LLVM_API_VERSION >= 700 */
#endif /* AMD64 || X86 */
#if defined(TARGET_WASM) && LLVM_API_VERSION >= 800
	case INTRINS_WASM_ANYTRUE_V16:
		intrins = add_intrins1 (module, id, sse_i1_t);
		break;
	case INTRINS_WASM_ANYTRUE_V8:
		intrins = add_intrins1 (module, id, sse_i2_t);
		break;
	case INTRINS_WASM_ANYTRUE_V4:
		intrins = add_intrins1 (module, id, sse_i4_t);
		break;
	case INTRINS_WASM_ANYTRUE_V2:
		intrins = add_intrins1 (module, id, sse_i8_t);
		break;
#endif
#ifdef TARGET_ARM64	
	case INTRINS_BITREVERSE_I32:	
		intrins = add_intrins1 (module, id, LLVMInt32Type ());	
		break;	
	case INTRINS_BITREVERSE_I64:	
		intrins = add_intrins1 (module, id, LLVMInt64Type ());	
		break;	
	case INTRINS_AARCH64_ADV_SIMD_ABS_FLOAT:
		intrins = add_intrins1 (module, id, sse_r4_t);
		break;
	case INTRINS_AARCH64_ADV_SIMD_ABS_DOUBLE:
		intrins = add_intrins1 (module, id, sse_r8_t);
		break;
	case INTRINS_AARCH64_ADV_SIMD_ABS_INT8:
		intrins = add_intrins1 (module, id, sse_i1_t);
		break;
	case INTRINS_AARCH64_ADV_SIMD_ABS_INT16:
		intrins = add_intrins1 (module, id, sse_i2_t);
		break;
	case INTRINS_AARCH64_ADV_SIMD_ABS_INT32:
		intrins = add_intrins1 (module, id, sse_i4_t);
		break;
	case INTRINS_AARCH64_ADV_SIMD_ABS_INT64:
		intrins = add_intrins1 (module, id, sse_i8_t);
		break;
#endif
	default:
		g_assert_not_reached ();
		break;
	}
	g_assert (intrins);
	g_hash_table_insert (intrins_id_to_intrins, GINT_TO_POINTER (id), intrins);
}

static LLVMValueRef
get_intrins_from_module (LLVMModuleRef lmodule, int id)
{
	LLVMValueRef res;

	res = static_cast<LLVMValueRef>(g_hash_table_lookup (intrins_id_to_intrins, GINT_TO_POINTER (id)));
	g_assert (res);
	return res;
}

LLVMValueRef
get_intrins (EmitContext *ctx, int id)
{
	MonoLLVMModule *module = ctx->module;
	LLVMValueRef res;

	/*
	 * Every method is emitted into its own module so
	 * we can add intrinsics on demand.
	 */
	res = module->intrins_by_id [id];
	if (!res) {
		res = get_intrins_from_module (ctx->lmodule, id);
		module->intrins_by_id [id] = res;
	}
	return res;
}

void
add_intrinsics (LLVMModuleRef module)
{
	int i;

	/* Emit declarations of instrinsics */
	/*
	 * It would be nicer to emit only the intrinsics actually used, but LLVM's Module
	 * type doesn't seem to do any locking.
	 */
	for (i = 0; i < INTRINS_NUM; ++i)
		add_intrinsic (module, static_cast<IntrinsicId>(i));

	/* EH intrinsics */
	add_func (module, "mono_personality", LLVMVoidType (), NULL, 0);
	add_func (module, "llvm_resume_unwind_trampoline", LLVMVoidType (), NULL, 0);
}

void
add_types (MonoLLVMModule *module)
{
	module->ptr_type = LLVMPointerType (TARGET_SIZEOF_VOID_P == 8 ? LLVMInt64Type () : LLVMInt32Type (), 0);
}


#endif /* DISABLE_JIT */
