/**
 * \file
 * translator-types.cpp: mono type, type enum and opcode -> LLVM type mapping.
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

void
EmitContext::set_failure (const char *message)
{
	TRACE_FAILURE (this, message);
	cfg->exception_message = g_strdup (message);
	cfg->disable_llvm = TRUE;
}

LLVMValueRef
const_int32 (int v)
{
	return llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt32Ty (llvm_global_ctx ()), v, false));
}

LLVMValueRef
const_int64 (int64_t v)
{
	return llvm::wrap (llvm::ConstantInt::get (llvm::Type::getInt64Ty (llvm_global_ctx ()), v, false));
}

/*
 * IntPtrType:
 *
 *   The LLVM type with width == TARGET_SIZEOF_VOID_P
 */
LLVMTypeRef
IntPtrType (void)
{
	return TARGET_SIZEOF_VOID_P == 8 ? llvm::wrap (llvm::Type::getInt64Ty (llvm_global_ctx ())) : llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ()));
}

LLVMTypeRef
ObjRefType (void)
{
	return TARGET_SIZEOF_VOID_P == 8 ? llvm::wrap (llvm::PointerType::get (llvm_global_ctx (), 0)) : llvm::wrap (llvm::PointerType::get (llvm_global_ctx (), 0));
}

LLVMTypeRef
ThisType (void)
{
	return TARGET_SIZEOF_VOID_P == 8 ? llvm::wrap (llvm::PointerType::get (llvm_global_ctx (), 0)) : llvm::wrap (llvm::PointerType::get (llvm_global_ctx (), 0));
}

/*
 * get_vtype_size:
 *
 *   Return the size of the LLVM representation of the vtype T.
 */
guint32
get_vtype_size (MonoType *t)
{
	int size;

	size = mono_class_value_size (mono_class_from_mono_type_internal (t), NULL);

	/* LLVMArgAsIArgs depends on this since it stores whole words */
	while (size < 2 * TARGET_SIZEOF_VOID_P && mono_is_power_of_two (size) == -1)
		size ++;

	return size;
}

/*
 * simd_class_to_llvm_type:
 *
 *   Return the LLVM type corresponding to the Mono.SIMD class KLASS
 */
LLVMTypeRef
simd_class_to_llvm_type (EmitContext *ctx, MonoClass *klass)
{
	const char *klass_name = m_class_get_name (klass);
	if (!strcmp (klass_name, "Vector2d")) {
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getDoubleTy (ctx->llvm_ctx ()), 2));
	} else if (!strcmp (klass_name, "Vector2l")) {
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt64Ty (ctx->llvm_ctx ()), 2));
	} else if (!strcmp (klass_name, "Vector2ul")) {
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt64Ty (ctx->llvm_ctx ()), 2));
	} else if (!strcmp (klass_name, "Vector4i")) {
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), 4));
	} else if (!strcmp (klass_name, "Vector4ui")) {
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), 4));
	} else if (!strcmp (klass_name, "Vector4f")) {
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getFloatTy (ctx->llvm_ctx ()), 4));
	} else if (!strcmp (klass_name, "Vector8s")) {
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt16Ty (ctx->llvm_ctx ()), 8));
	} else if (!strcmp (klass_name, "Vector8us")) {
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt16Ty (ctx->llvm_ctx ()), 8));
	} else if (!strcmp (klass_name, "Vector16sb")) {
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt8Ty (ctx->llvm_ctx ()), 16));
	} else if (!strcmp (klass_name, "Vector16b")) {
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt8Ty (ctx->llvm_ctx ()), 16));
	} else if (!strcmp (klass_name, "Vector2")) {
		/* System.Numerics */
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getFloatTy (ctx->llvm_ctx ()), 4));
	} else if (!strcmp (klass_name, "Vector3")) {
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getFloatTy (ctx->llvm_ctx ()), 4));
	} else if (!strcmp (klass_name, "Vector4")) {
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getFloatTy (ctx->llvm_ctx ()), 4));
	} else if (!strcmp (klass_name, "Vector`1") || !strcmp (klass_name, "Vector128`1") || !strcmp (klass_name, "Vector256`1")) {
		MonoType *etype = mono_class_get_generic_class (klass)->context.class_inst->type_argv [0];
		int size = mono_class_value_size (klass, NULL);
		switch (etype->type) {
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
			return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt8Ty (ctx->llvm_ctx ()), size));
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
			return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt16Ty (ctx->llvm_ctx ()), size / 2));
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
			return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt32Ty (ctx->llvm_ctx ()), size / 4));
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
			return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt64Ty (ctx->llvm_ctx ()), size / 8));
		case MONO_TYPE_R4:
			return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getFloatTy (ctx->llvm_ctx ()), size / 4));
		case MONO_TYPE_R8:
			return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getDoubleTy (ctx->llvm_ctx ()), size / 8));
		default:
			g_assert_not_reached ();
			return nullptr;
		}
	} else {
		printf ("%s\n", klass_name);
		NOT_IMPLEMENTED;
		return nullptr;
	}
}

/* Return the 128 bit SIMD type corresponding to the mono type TYPE */
G_GNUC_UNUSED LLVMTypeRef
type_to_sse_type (int type)
{
	switch (type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt8Ty (llvm_global_ctx ()), 16));
	case MONO_TYPE_U2:
	case MONO_TYPE_I2:
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt16Ty (llvm_global_ctx ()), 8));
	case MONO_TYPE_U4:
	case MONO_TYPE_I4:
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt32Ty (llvm_global_ctx ()), 4));
	case MONO_TYPE_U8:
	case MONO_TYPE_I8:
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getInt64Ty (llvm_global_ctx ()), 2));
	case MONO_TYPE_R8:
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getDoubleTy (llvm_global_ctx ()), 2));
	case MONO_TYPE_R4:
		return llvm::wrap (llvm::FixedVectorType::get (llvm::Type::getFloatTy (llvm_global_ctx ()), 4));
	default:
		g_assert_not_reached ();
		return nullptr;
	}
}

static LLVMTypeRef
create_llvm_type_for_type (MonoLLVMModule *module, MonoClass *klass)
{
	int i, size, nfields, esize;
	LLVMTypeRef *eltypes;
	char *name;
	MonoType *t;
	LLVMTypeRef ltype;

	t = m_class_get_byval_arg (klass);

	if (mini_type_is_hfa (t, &nfields, &esize)) {
		/*
		 * This is needed on arm64 where HFAs are returned in
		 * registers.
		 */
		/* SIMD types have size 16 in mono_class_value_size () */
		if (m_class_is_simd_type (klass))
			nfields = 16/ esize;
		size = nfields;
		eltypes = g_new (LLVMTypeRef, size);
		for (i = 0; i < size; ++i)
			eltypes [i] = esize == 4 ? llvm::wrap (llvm::Type::getFloatTy (llvm_global_ctx ())) : llvm::wrap (llvm::Type::getDoubleTy (llvm_global_ctx ()));
	} else {
		size = get_vtype_size (t);

		eltypes = g_new (LLVMTypeRef, size);
		for (i = 0; i < size; ++i)
			eltypes [i] = llvm::wrap (llvm::Type::getInt8Ty (llvm_global_ctx ()));
	}

	name = mono_type_full_name (m_class_get_byval_arg (klass));
	ltype = LLVMStructCreateNamed (module->context, name);
	LLVMStructSetBody (ltype, eltypes, size, FALSE);
	g_free (eltypes);
	g_free (name);

	return ltype;
}

LLVMTypeRef
primitive_type_to_llvm_type (MonoTypeEnum type)
{
	switch (type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
		return llvm::wrap (llvm::Type::getInt8Ty (llvm_global_ctx ()));
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
		return llvm::wrap (llvm::Type::getInt16Ty (llvm_global_ctx ()));
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
		return llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ()));
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		return llvm::wrap (llvm::Type::getInt64Ty (llvm_global_ctx ()));
	case MONO_TYPE_R4:
		return llvm::wrap (llvm::Type::getFloatTy (llvm_global_ctx ()));
	case MONO_TYPE_R8:
		return llvm::wrap (llvm::Type::getDoubleTy (llvm_global_ctx ()));
	case MONO_TYPE_I:
	case MONO_TYPE_U:
		return IntPtrType ();
	default:
		return nullptr;
	}
}

MonoTypeEnum
inst_c1_type (const MonoInst *ins)
{
	return static_cast<MonoTypeEnum>(ins->inst_c1);
}

/*
 * type_to_llvm_type:
 *
 *   Return the LLVM type corresponding to T.
 */
LLVMTypeRef
type_to_llvm_type (EmitContext *ctx, MonoType *t)
{
	if (t->byref)
		return ThisType ();

	t = mini_get_underlying_type (t);

	LLVMTypeRef prim_llvm_type = primitive_type_to_llvm_type (t->type);
	if (prim_llvm_type != nullptr)
		return prim_llvm_type;

	switch (t->type) {
	case MONO_TYPE_VOID:
		return llvm::wrap (llvm::Type::getVoidTy (ctx->llvm_ctx ()));
	case MONO_TYPE_OBJECT:
		return ObjRefType ();
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR: {
		MonoClass *klass = mono_class_from_mono_type_internal (t);
		MonoClass *ptr_klass = m_class_get_element_class (klass);
		MonoType *ptr_type = m_class_get_byval_arg (ptr_klass);
		/* Handle primitive pointers  */
		switch (ptr_type->type) {
		case MONO_TYPE_I1:
		case MONO_TYPE_I2:
		case MONO_TYPE_I4:
		case MONO_TYPE_U1:
		case MONO_TYPE_U2:
		case MONO_TYPE_U4:
			return llvm::wrap (llvm::PointerType::get (ctx->llvm_ctx (), 0));
		default:
			break;
		}
		
		return ObjRefType ();
	}
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		/* Because of generic sharing */
		return ObjRefType ();
	case MONO_TYPE_GENERICINST:
		if (!mono_type_generic_inst_is_valuetype (t))
			return ObjRefType ();
		/* Fall through */
	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_TYPEDBYREF: {
		MonoClass *klass;
		LLVMTypeRef ltype;

		klass = mono_class_from_mono_type_internal (t);

		if (MONO_CLASS_IS_SIMD (ctx->cfg, klass))
			return simd_class_to_llvm_type (ctx, klass);

		if (m_class_is_enumtype (klass))
			return type_to_llvm_type (ctx, mono_class_enum_basetype_internal (klass));

		ltype = static_cast<LLVMTypeRef>(g_hash_table_lookup (ctx->module->llvm_types, klass));
		if (!ltype) {
			ltype = create_llvm_type_for_type (ctx->module, klass);
			g_hash_table_insert (ctx->module->llvm_types, klass, ltype);
		}
		return ltype;
	}

	default:
		printf ("X: %d\n", t->type);
		ctx->cfg->exception_message = g_strdup_printf ("type %s", mono_type_full_name (t));
		ctx->cfg->disable_llvm = TRUE;
		return nullptr;
	}
}

bool
primitive_type_is_unsigned (MonoTypeEnum t)
{
	switch (t) {
	case MONO_TYPE_U1:
	case MONO_TYPE_U2:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U4:
	case MONO_TYPE_U8:
		return true;
	default:
		return false;
	}
}

/*
 * type_is_unsigned:
 *
 *   Return whenever T is an unsigned int type.
 */
bool
type_is_unsigned (EmitContext *ctx, MonoType *t)
{
	t = mini_get_underlying_type (t);
	if (t->byref)
		return false;
	return primitive_type_is_unsigned (t->type);
}

/*
 * type_to_llvm_arg_type:
 *
 *   Same as type_to_llvm_type, but treat i8/i16 as i32.
 */
LLVMTypeRef
type_to_llvm_arg_type (EmitContext *ctx, MonoType *t)
{
	LLVMTypeRef ptype = type_to_llvm_type (ctx, t);

	/*
	 * This works on all abis except arm64/ios which passes multiple
	 * arguments in one stack slot.
	 */
#ifndef TARGET_ARM64
	if (ptype == llvm::wrap (llvm::Type::getInt8Ty (ctx->llvm_ctx ())) || ptype == llvm::wrap (llvm::Type::getInt16Ty (ctx->llvm_ctx ()))) {
		/* 
		 * LLVM generates code which only sets the lower bits, while JITted
		 * code expects all the bits to be set.
		 */
		ptype = llvm::wrap (llvm::Type::getInt32Ty (ctx->llvm_ctx ()));
	}
#endif

	return ptype;
}

/*
 * llvm_type_to_stack_type:
 *
 *   Return the LLVM type which needs to be used when a value of type TYPE is pushed
 * on the IL stack.
 */
G_GNUC_UNUSED LLVMTypeRef
llvm_type_to_stack_type (MonoCompile *cfg, LLVMTypeRef type)
{
	if (type == nullptr)
		return nullptr;
	if (type == llvm::wrap (llvm::Type::getInt8Ty (llvm_global_ctx ())))
		return llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ()));
	else if (type == llvm::wrap (llvm::Type::getInt16Ty (llvm_global_ctx ())))
		return llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ()));
	else if (!cfg->r4fp && type == llvm::wrap (llvm::Type::getFloatTy (llvm_global_ctx ())))
		return llvm::wrap (llvm::Type::getDoubleTy (llvm_global_ctx ()));
	else
		return type;
}

/*
 * regtype_to_llvm_type:
 *
 *   Return the LLVM type corresponding to the regtype C used in instruction 
 * descriptions.
 */
LLVMTypeRef
regtype_to_llvm_type (char c)
{
	switch (c) {
	case 'i':
		return llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ()));
	case 'l':
		return llvm::wrap (llvm::Type::getInt64Ty (llvm_global_ctx ()));
	case 'f':
		return llvm::wrap (llvm::Type::getDoubleTy (llvm_global_ctx ()));
	default:
		return nullptr;
	}
}

/*
 * op_to_llvm_type:
 *
 *   Return the LLVM type corresponding to the unary/binary opcode OPCODE.
 */
LLVMTypeRef
op_to_llvm_type (int opcode)
{
	switch (opcode) {
	case OP_ICONV_TO_I1:
	case OP_LCONV_TO_I1:
		return llvm::wrap (llvm::Type::getInt8Ty (llvm_global_ctx ()));
	case OP_ICONV_TO_U1:
	case OP_LCONV_TO_U1:
		return llvm::wrap (llvm::Type::getInt8Ty (llvm_global_ctx ()));
	case OP_ICONV_TO_I2:
	case OP_LCONV_TO_I2:
		return llvm::wrap (llvm::Type::getInt16Ty (llvm_global_ctx ()));
	case OP_ICONV_TO_U2:
	case OP_LCONV_TO_U2:
		return llvm::wrap (llvm::Type::getInt16Ty (llvm_global_ctx ()));
	case OP_ICONV_TO_I4:
	case OP_LCONV_TO_I4:
		return llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ()));
	case OP_ICONV_TO_U4:
	case OP_LCONV_TO_U4:
		return llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ()));
	case OP_ICONV_TO_I8:
		return llvm::wrap (llvm::Type::getInt64Ty (llvm_global_ctx ()));
	case OP_ICONV_TO_R4:
		return llvm::wrap (llvm::Type::getFloatTy (llvm_global_ctx ()));
	case OP_ICONV_TO_R8:
		return llvm::wrap (llvm::Type::getDoubleTy (llvm_global_ctx ()));
	case OP_ICONV_TO_U8:
		return llvm::wrap (llvm::Type::getInt64Ty (llvm_global_ctx ()));
	case OP_FCONV_TO_I4:
		return llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ()));
	case OP_FCONV_TO_I8:
		return llvm::wrap (llvm::Type::getInt64Ty (llvm_global_ctx ()));
	case OP_FCONV_TO_I1:
	case OP_FCONV_TO_U1:
	case OP_RCONV_TO_I1:
	case OP_RCONV_TO_U1:
		return llvm::wrap (llvm::Type::getInt8Ty (llvm_global_ctx ()));
	case OP_FCONV_TO_I2:
	case OP_FCONV_TO_U2:
	case OP_RCONV_TO_I2:
	case OP_RCONV_TO_U2:
		return llvm::wrap (llvm::Type::getInt16Ty (llvm_global_ctx ()));
	case OP_FCONV_TO_U4:
	case OP_RCONV_TO_U4:
		return llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ()));
	case OP_FCONV_TO_U8:
	case OP_RCONV_TO_U8:
		return llvm::wrap (llvm::Type::getInt64Ty (llvm_global_ctx ()));
	case OP_FCONV_TO_I:
	case OP_FCONV_TO_U:
		return TARGET_SIZEOF_VOID_P == 8 ? llvm::wrap (llvm::Type::getInt64Ty (llvm_global_ctx ())) : llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ()));
	case OP_IADD_OVF:
	case OP_IADD_OVF_UN:
	case OP_ISUB_OVF:
	case OP_ISUB_OVF_UN:
	case OP_IMUL_OVF:
	case OP_IMUL_OVF_UN:
		return llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ()));
	case OP_LADD_OVF:
	case OP_LADD_OVF_UN:
	case OP_LSUB_OVF:
	case OP_LSUB_OVF_UN:
	case OP_LMUL_OVF:
	case OP_LMUL_OVF_UN:
		return llvm::wrap (llvm::Type::getInt64Ty (llvm_global_ctx ()));
	default:
		printf ("%s\n", mono_inst_name (opcode));
		g_assert_not_reached ();
		return nullptr;
	}
}		

#define CLAUSE_START(clause) ((clause)->try_offset)
#define CLAUSE_END(clause) (((clause))->try_offset + ((clause))->try_len)

/*
 * load_store_to_llvm_type:
 *
 *   Return the size/sign/zero extension corresponding to the load/store opcode
 * OPCODE.
 */
LLVMTypeRef
load_store_to_llvm_type (int opcode, int *size, gboolean *sext, gboolean *zext)
{
	*sext = FALSE;
	*zext = FALSE;

	switch (opcode) {
	case OP_LOADI1_MEMBASE:
	case OP_STOREI1_MEMBASE_REG:
	case OP_STOREI1_MEMBASE_IMM:
	case OP_ATOMIC_LOAD_I1:
	case OP_ATOMIC_STORE_I1:
		*size = 1;
		*sext = TRUE;
		return llvm::wrap (llvm::Type::getInt8Ty (llvm_global_ctx ()));
	case OP_LOADU1_MEMBASE:
	case OP_LOADU1_MEM:
	case OP_ATOMIC_LOAD_U1:
	case OP_ATOMIC_STORE_U1:
		*size = 1;
		*zext = TRUE;
		return llvm::wrap (llvm::Type::getInt8Ty (llvm_global_ctx ()));
	case OP_LOADI2_MEMBASE:
	case OP_STOREI2_MEMBASE_REG:
	case OP_STOREI2_MEMBASE_IMM:
	case OP_ATOMIC_LOAD_I2:
	case OP_ATOMIC_STORE_I2:
		*size = 2;
		*sext = TRUE;
		return llvm::wrap (llvm::Type::getInt16Ty (llvm_global_ctx ()));
	case OP_LOADU2_MEMBASE:
	case OP_LOADU2_MEM:
	case OP_ATOMIC_LOAD_U2:
	case OP_ATOMIC_STORE_U2:
		*size = 2;
		*zext = TRUE;
		return llvm::wrap (llvm::Type::getInt16Ty (llvm_global_ctx ()));
	case OP_LOADI4_MEMBASE:
	case OP_LOADU4_MEMBASE:
	case OP_LOADI4_MEM:
	case OP_LOADU4_MEM:
	case OP_STOREI4_MEMBASE_REG:
	case OP_STOREI4_MEMBASE_IMM:
	case OP_ATOMIC_LOAD_I4:
	case OP_ATOMIC_STORE_I4:
	case OP_ATOMIC_LOAD_U4:
	case OP_ATOMIC_STORE_U4:
		*size = 4;
		return llvm::wrap (llvm::Type::getInt32Ty (llvm_global_ctx ()));
	case OP_LOADI8_MEMBASE:
	case OP_LOADI8_MEM:
	case OP_STOREI8_MEMBASE_REG:
	case OP_STOREI8_MEMBASE_IMM:
	case OP_ATOMIC_LOAD_I8:
	case OP_ATOMIC_STORE_I8:
	case OP_ATOMIC_LOAD_U8:
	case OP_ATOMIC_STORE_U8:
		*size = 8;
		return llvm::wrap (llvm::Type::getInt64Ty (llvm_global_ctx ()));
	case OP_LOADR4_MEMBASE:
	case OP_STORER4_MEMBASE_REG:
	case OP_ATOMIC_LOAD_R4:
	case OP_ATOMIC_STORE_R4:
		*size = 4;
		return llvm::wrap (llvm::Type::getFloatTy (llvm_global_ctx ()));
	case OP_LOADR8_MEMBASE:
	case OP_STORER8_MEMBASE_REG:
	case OP_ATOMIC_LOAD_R8:
	case OP_ATOMIC_STORE_R8:
		*size = 8;
		return llvm::wrap (llvm::Type::getDoubleTy (llvm_global_ctx ()));
	case OP_LOAD_MEMBASE:
	case OP_LOAD_MEM:
	case OP_STORE_MEMBASE_REG:
	case OP_STORE_MEMBASE_IMM:
		*size = TARGET_SIZEOF_VOID_P;
		return IntPtrType ();
	default:
		g_assert_not_reached ();
		return nullptr;
	}
}

/*
 * ovf_op_to_intrins:
 *
 *   Return the LLVM intrinsics corresponding to the overflow opcode OPCODE.
 */
IntrinsicId
ovf_op_to_intrins (int opcode)
{
	switch (opcode) {
	case OP_IADD_OVF:
		return INTRINS_SADD_OVF_I32;
	case OP_IADD_OVF_UN:
		return INTRINS_UADD_OVF_I32;
	case OP_ISUB_OVF:
		return INTRINS_SSUB_OVF_I32;
	case OP_ISUB_OVF_UN:
		return INTRINS_USUB_OVF_I32;
	case OP_IMUL_OVF:
		return INTRINS_SMUL_OVF_I32;
	case OP_IMUL_OVF_UN:
		return INTRINS_UMUL_OVF_I32;
	case OP_LADD_OVF:
		return INTRINS_SADD_OVF_I64;
	case OP_LADD_OVF_UN:
		return INTRINS_UADD_OVF_I64;
	case OP_LSUB_OVF:
		return INTRINS_SSUB_OVF_I64;
	case OP_LSUB_OVF_UN:
		return INTRINS_USUB_OVF_I64;
	case OP_LMUL_OVF:
		return INTRINS_SMUL_OVF_I64;
	case OP_LMUL_OVF_UN:
		return INTRINS_UMUL_OVF_I64;
	default:
		g_assert_not_reached ();
		return static_cast<IntrinsicId>(0);
	}
}

IntrinsicId
simd_ins_to_intrins (int opcode)
{
	switch (opcode) {
#if defined(TARGET_X86) || defined(TARGET_AMD64)
	case OP_MINPD:
		return INTRINS_SSE_MINPD;
	case OP_MINPS:
		return INTRINS_SSE_MINPS;
	case OP_MAXPD:
		return INTRINS_SSE_MAXPD;
	case OP_MAXPS:
		return INTRINS_SSE_MAXPS;
	case OP_HADDPD:
		return INTRINS_SSE_HADDPD;
	case OP_HADDPS:
		return INTRINS_SSE_HADDPS;
	case OP_HSUBPD:
		return INTRINS_SSE_HSUBPD;
	case OP_HSUBPS:
		return INTRINS_SSE_HSUBPS;
	case OP_ADDSUBPS:
		return INTRINS_SSE_ADDSUBPS;
	case OP_ADDSUBPD:
		return INTRINS_SSE_ADDSUBPD;
	case OP_EXTRACT_MASK:
		return INTRINS_SSE_PMOVMSKB;
	case OP_PSHRW:
	case OP_PSHRW_REG:
		return INTRINS_SSE_PSRLI_W;
	case OP_PSHRD:
	case OP_PSHRD_REG:
		return INTRINS_SSE_PSRLI_D;
	case OP_PSHRQ:
	case OP_PSHRQ_REG:
		return INTRINS_SSE_PSRLI_Q;
	case OP_PSHLW:
	case OP_PSHLW_REG:
		return INTRINS_SSE_PSLLI_W;
	case OP_PSHLD:
	case OP_PSHLD_REG:
		return INTRINS_SSE_PSLLI_D;
	case OP_PSHLQ:
	case OP_PSHLQ_REG:
		return INTRINS_SSE_PSLLI_Q;
	case OP_PSARW:
	case OP_PSARW_REG:
		return INTRINS_SSE_PSRAI_W;
	case OP_PSARD:
	case OP_PSARD_REG:
		return INTRINS_SSE_PSRAI_D;
	case OP_RSQRTPS:
		return INTRINS_SSE_RSQRT_PS;
	case OP_RCPPS:
		return INTRINS_SSE_RCP_PS;
	case OP_CVTPD2DQ:
		return INTRINS_SSE_CVTPD2DQ;
	case OP_CVTPS2DQ:
		return INTRINS_SSE_CVTPS2DQ;
	case OP_CVTPD2PS:
		return INTRINS_SSE_CVTPD2PS;
	case OP_CVTTPD2DQ:
		return INTRINS_SSE_CVTTPD2DQ;
	case OP_CVTTPS2DQ:
		return INTRINS_SSE_CVTTPS2DQ;
	case OP_PACKW:
		return INTRINS_SSE_PACKSSWB;
	case OP_PACKD:
		return INTRINS_SSE_PACKSSDW;
	case OP_PACKW_UN:
		return INTRINS_SSE_PACKUSWB;
	case OP_PACKD_UN:
		return INTRINS_SSE_PACKUSDW;
	case OP_PMULW_HIGH:
		return INTRINS_SSE_PMULHW;
	case OP_PMULW_HIGH_UN:
		return INTRINS_SSE_PMULHU;
	case OP_DPPS:
		return INTRINS_SSE_DPPS;
	case OP_SSE_SQRTSS:
		return INTRINS_SSE_SQRT_SS;
	case OP_SSE2_SQRTSD:
		return INTRINS_SSE_SQRT_SD;
	case OP_SQRTPS:
		return INTRINS_SSE_SQRT_PS;
	case OP_SQRTPD:
		return INTRINS_SSE_SQRT_PD;
#endif
	default:
		g_assert_not_reached ();
		return static_cast<IntrinsicId>(0);
	}
}

LLVMTypeRef
simd_op_to_llvm_type (int opcode)
{
#if defined(TARGET_X86) || defined(TARGET_AMD64)
	switch (opcode) {
	case OP_EXTRACT_R8:
	case OP_EXPAND_R8:
		return sse_r8_t;
	case OP_EXTRACT_I8:
	case OP_EXPAND_I8:
		return sse_i8_t;
	case OP_EXTRACT_I4:
	case OP_EXPAND_I4:
		return sse_i4_t;
	case OP_EXTRACT_I2:
	case OP_EXTRACT_U2:
	case OP_EXTRACTX_U2:
	case OP_EXPAND_I2:
		return sse_i2_t;
	case OP_EXTRACT_I1:
	case OP_EXTRACT_U1:
	case OP_EXPAND_I1:
		return sse_i1_t;
	case OP_EXTRACT_R4:
	case OP_EXPAND_R4:
		return sse_r4_t;
	case OP_CVTPD2DQ:
	case OP_CVTPD2PS:
	case OP_CVTTPD2DQ:
		return sse_r8_t;
	case OP_CVTPS2DQ:
	case OP_CVTTPS2DQ:
		return sse_r4_t;
	case OP_EXTRACT_MASK:
		return sse_i1_t;
	case OP_SQRTPS:
	case OP_RSQRTPS:
	case OP_RCPPS:
	case OP_DUPPS_LOW:
	case OP_DUPPS_HIGH:
		return sse_r4_t;
	case OP_SQRTPD:
	case OP_DUPPD:
		return sse_r8_t;
	default:
		g_assert_not_reached ();
		return nullptr;
	}
#else
	return nullptr;
#endif
}

void
set_cold_cconv (LLVMValueRef func)
{
	/*
	 * xcode10 (watchOS) and ARM/ARM64 doesn't seem to support preserveall, it fails with:
	 * fatal error: error in backend: Unsupported calling convention
	 */
#if !defined(TARGET_WATCHOS) && !defined(TARGET_ARM) && !defined(TARGET_ARM64)
	LLVMSetFunctionCallConv (func, LLVMColdCallConv);
#endif
}

void
set_call_cold_cconv (LLVMValueRef func)
{
#if !defined(TARGET_WATCHOS) && !defined(TARGET_ARM) && !defined(TARGET_ARM64)
	LLVMSetInstructionCallConv (func, LLVMColdCallConv);
#endif
}

#endif /* DISABLE_JIT */
