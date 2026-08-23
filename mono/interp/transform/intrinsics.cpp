/**
 * \file
 * \brief Library methods the transform answers with an opcode of its own.
 *
 * A call site whose target is one of these does not become a call. What
 * replaces it is either a single MINT opcode or a short sequence, so the
 * interpreter never enters the callee.
 */

#include "config.h"

#include <mono/metadata/abi-details.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/tabledefs.h>
#include <mono/utils/mono-memory-model.h>

#include <mono/mini/mini-runtime.h>

#include "mintops.hpp"
#include "runtime/internals.hpp"
#include "interp.h"
#include "transform.hpp"
#include "internal.hpp"

#if SIZEOF_VOID_P == 8
#define MINT_NEG_P MINT_NEG_I8
#define MINT_NOT_P MINT_NOT_I8

#define MINT_MOV_FP MINT_MOV_8
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

#define MINT_MOV_FP MINT_MOV_4
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

namespace mono::interp {

struct MagicIntrinsic {
	const gchar *op_name;
	guint16 insn[3];
};

static const MagicIntrinsic int_unnop[] = {
	{"op_UnaryPlus", {MINT_MOV_P, MINT_MOV_P, MINT_MOV_FP}},
	{"op_UnaryNegation", {MINT_NEG_P, MINT_NEG_P, MINT_NEG_FP}},
	{"op_OnesComplement", {MINT_NOT_P, MINT_NOT_P, MINT_NIY}}};

static const MagicIntrinsic int_binop[] = {
	{"op_Addition", {MINT_ADD_P, MINT_ADD_P, MINT_ADD_FP}},
	{"op_Subtraction", {MINT_SUB_P, MINT_SUB_P, MINT_SUB_FP}},
	{"op_Multiply", {MINT_MUL_P, MINT_MUL_P, MINT_MUL_FP}},
	{"op_Division", {MINT_DIV_P, MINT_DIV_UN_P, MINT_DIV_FP}},
	{"op_Modulus", {MINT_REM_P, MINT_REM_UN_P, MINT_REM_FP}},
	{"op_BitwiseAnd", {MINT_AND_P, MINT_AND_P, MINT_NIY}},
	{"op_BitwiseOr", {MINT_OR_P, MINT_OR_P, MINT_NIY}},
	{"op_ExclusiveOr", {MINT_XOR_P, MINT_XOR_P, MINT_NIY}},
	{"op_LeftShift", {MINT_SHL_P, MINT_SHL_P, MINT_NIY}},
	{"op_RightShift", {MINT_SHR_P, MINT_SHR_UN_P, MINT_NIY}},
};

static const MagicIntrinsic int_cmpop[] = {
	{"op_Inequality", {MINT_CNE_P, MINT_CNE_P, MINT_CNE_FP}},
	{"op_Equality", {MINT_CEQ_P, MINT_CEQ_P, MINT_CEQ_FP}},
	{"op_GreaterThan", {MINT_CGT_P, MINT_CGT_UN_P, MINT_CGT_FP}},
	{"op_GreaterThanOrEqual", {MINT_CGE_P, MINT_CGE_UN_P, MINT_CGE_FP}},
	{"op_LessThan", {MINT_CLT_P, MINT_CLT_UN_P, MINT_CLT_FP}},
	{"op_LessThanOrEqual", {MINT_CLE_P, MINT_CLE_UN_P, MINT_CLE_FP}}};

gboolean
TransformData::interp_handle_magic_type_intrinsics (MonoMethod *target_method,
                                                    MonoMethodSignature *csignature, int type_index)
{
	MonoClass *magic_class = target_method->klass;
	const char *tm = target_method->name;
	int i;

	const MintType mt = mint_type (m_class_get_byval_arg (magic_class));
	if (!strcmp (".ctor", tm)) {
		MonoType *arg = csignature->params[0];
		/* Converts the argument to match SIZEOF_VOID_P's width, when its own width
		 * differs. */
		int arg_size = mini_magic_type_size (arg);

		if (arg_size > SIZEOF_VOID_P) { // 8 -> 4
			switch (type_index) {
			case 0:
			case 1:
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
		case 0:
		case 1:
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
		interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
		ip += 5;
		return TRUE;
	} else if (!strcmp ("op_Implicit", tm) || !strcmp ("op_Explicit", tm)) {
		MonoType *src = csignature->params[0];
		MonoType *dst = csignature->ret;
		MonoClass *src_klass = mono_class_from_mono_type_internal (src);
		int src_size = mini_magic_type_size (src);
		int dst_size = mini_magic_type_size (dst);

		gboolean keeps_managed_body = FALSE;

		switch (type_index) {
		case 0:
		case 1:
			if (!mini_magic_is_int_type (src) || !mini_magic_is_int_type (dst)) {
				if (mini_magic_is_int_type (src))
					keeps_managed_body = TRUE;
				else if (mono_class_is_magic_float (src_klass))
					keeps_managed_body = TRUE;
				else
					return FALSE;
			}
			break;
		case 2:
			if (!mini_magic_is_float_type (src) || !mini_magic_is_float_type (dst)) {
				if (mini_magic_is_float_type (src))
					keeps_managed_body = TRUE;
				else if (mono_class_is_magic_int (src_klass))
					keeps_managed_body = TRUE;
				else
					return FALSE;
			}
			break;
		}

		if (keeps_managed_body)
			return FALSE;

		if (src_size > dst_size) { // 8 -> 4
			switch (type_index) {
			case 0:
			case 1:
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

		SET_TYPE (sp - 1, stack_type_of (mint_type (dst)),
		          mono_class_from_mono_type_internal (dst));
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
		interp_ins_set_sreg (last_ins, sp[0].local);
		push_type (stack_type_of (mt), magic_class);
		interp_ins_set_dreg (last_ins, sp[-1].local);
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
		interp_ins_set_sreg (last_ins, sp[0].local);
		push_type (stack_type_of (mt), magic_class);
		interp_ins_set_dreg (last_ins, sp[-1].local);
		ip += 5;
		return TRUE;
	} else if (!strcmp ("CompareTo", tm) || !strcmp ("Equals", tm)) {
		return FALSE;
	} else if (!strcmp (".cctor", tm)) {
		return FALSE;
	} else if (!strcmp ("Parse", tm)) {
		return FALSE;
	} else if (!strcmp ("ToString", tm)) {
		return FALSE;
	} else if (!strcmp ("GetHashCode", tm)) {
		return FALSE;
	} else if (!strcmp ("IsNaN", tm) || !strcmp ("IsInfinity", tm)
	           || !strcmp ("IsNegativeInfinity", tm) || !strcmp ("IsPositiveInfinity", tm)) {
		g_assert (type_index == 2); // nfloat only
		return FALSE;
	}

	for (i = 0; i < sizeof (int_unnop) / sizeof (MagicIntrinsic); ++i) {
		if (!strcmp (int_unnop[i].op_name, tm)) {
			interp_add_ins (int_unnop[i].insn[type_index]);
			sp--;
			interp_ins_set_sreg (last_ins, sp[0].local);
			push_type (stack_type_of (mt), magic_class);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 5;
			return TRUE;
		}
	}

	for (i = 0; i < sizeof (int_binop) / sizeof (MagicIntrinsic); ++i) {
		if (!strcmp (int_binop[i].op_name, tm)) {
			interp_add_ins (int_binop[i].insn[type_index]);
			sp -= 2;
			interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
			push_type (stack_type_of (mt), magic_class);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 5;
			return TRUE;
		}
	}

	for (i = 0; i < sizeof (int_cmpop) / sizeof (MagicIntrinsic); ++i) {
		if (!strcmp (int_cmpop[i].op_name, tm)) {
			MonoClass *k = mono_defaults.boolean_class;
			interp_add_ins (int_cmpop[i].insn[type_index]);
			sp -= 2;
			interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
			push_type (stack_type_of (mint_type (m_class_get_byval_arg (k))), k);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 5;
			return TRUE;
		}
	}

	g_error ("TODO: interp_transform_call %s:%s", m_class_get_name (target_method->klass), tm);
}

/// Whether target_method's call was fully replaced by emitted bytecode.
/// Otherwise *op can carry a MINT opcode to emit in place of the call, or be
/// left unchanged for an ordinary call.
gboolean
TransformData::interp_handle_intrinsics (MonoMethod *target_method, MonoClass *constrained_class,
                                         MonoMethodSignature *csignature, gboolean readonly,
                                         int *op)
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
		if (tm[0] == 'g') {
			if (strcmp (tm, "get_Chars") == 0)
				*op = MINT_GETCHR;
			else if (strcmp (tm, "get_Length") == 0)
				*op = MINT_STRLEN;
		}
	} else if (type_index >= 0) {
		return interp_handle_magic_type_intrinsics (target_method, csignature, type_index);
	} else if (mono_class_is_subclass_of_internal (target_method->klass, mono_defaults.array_class,
	                                               FALSE)) {
		if (!strcmp (tm, "get_Rank")) {
			*op = MINT_ARRAY_RANK;
		} else if (!strcmp (tm, "get_Length")) {
			*op = MINT_LDLEN;
		} else if (!strcmp (tm, "GetElementSize")) {
			*op = MINT_ARRAY_ELEMENT_SIZE;
		} else if (!strcmp (tm, "IsPrimitive")) {
			*op = MINT_ARRAY_IS_PRIMITIVE;
		} else if (!strcmp (tm, "Address")) {
			MonoClass *check_class =
				readonly ? NULL : m_class_get_element_class (target_method->klass);
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
			MonoClass *value_class = sp[-1].klass;
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
	} else if (in_corlib && !strcmp (klass_name_space, "System.Diagnostics")
	           && !strcmp (klass_name, "Debugger")) {
		if (!strcmp (tm, "Break") && csignature->param_count == 0) {
			if (mini_should_insert_breakpoint (method))
				*op = MINT_BREAK;
		}
	} else if (in_corlib && !strcmp (klass_name_space, "System")
	           && !strcmp (klass_name, "SpanHelpers") && !strcmp (tm, "ClearWithReferences")) {
		*op = MINT_INTRINS_CLEAR_WITH_REFERENCES;
	} else if (in_corlib && !strcmp (klass_name_space, "System")
	           && !strcmp (klass_name, "ByReference`1")) {
		g_assert (!strcmp (tm, "get_Value"));
		*op = MINT_INTRINS_BYREFERENCE_GET_VALUE;
	} else if (in_corlib && !strcmp (klass_name_space, "System")
	           && !strcmp (klass_name, "Marvin")) {
		if (!strcmp (tm, "Block"))
			*op = MINT_INTRINS_MARVIN_BLOCK;
	} else if (in_corlib && !strcmp (klass_name_space, "System.Runtime.InteropServices")
	           && !strcmp (klass_name, "MemoryMarshal")) {
		if (!strcmp (tm, "GetArrayDataReference"))
			*op = MINT_INTRINS_MEMORYMARSHAL_GETARRAYDATAREF;
	} else if (in_corlib && !strcmp (klass_name_space, "System.Text.Unicode")
	           && !strcmp (klass_name, "Utf16Utility")) {
		if (!strcmp (tm, "ConvertAllAsciiCharsInUInt32ToUppercase"))
			*op = MINT_INTRINS_ASCII_CHARS_TO_UPPERCASE;
		else if (!strcmp (tm, "UInt32OrdinalIgnoreCaseAscii"))
			*op = MINT_INTRINS_ORDINAL_IGNORE_CASE_ASCII;
		else if (!strcmp (tm, "UInt64OrdinalIgnoreCaseAscii"))
			*op = MINT_INTRINS_64ORDINAL_IGNORE_CASE_ASCII;
	} else if (in_corlib && !strcmp (klass_name_space, "System.Text")
	           && !strcmp (klass_name, "ASCIIUtility")) {
		if (!strcmp (tm, "WidenAsciiToUtf16"))
			*op = MINT_INTRINS_WIDEN_ASCII_TO_UTF16;
	} else if (in_corlib && !strcmp (klass_name_space, "System")
	           && !strcmp (klass_name, "Number")) {
		if (!strcmp (tm, "UInt32ToDecStr") && csignature->param_count == 1) {
			ERROR_DECL (error);
			MonoVTable *vtable =
				mono_class_vtable_checked (rtm->domain, target_method->klass, error);
			if (!is_ok (error)) {
				mono_interp_error_cleanup (error);
				return FALSE;
			}
			/* Use the intrinsic only after the class constructor has run. */
			if (!vtable->initialized)
				return FALSE;
			/* The cache is the first static field. Update this if the BCL's layout
			 * changes. */
			MonoClassField *field = m_class_get_fields (target_method->klass);
			g_assert (!strcmp (field->name, "s_singleDigitStringCache"));
			interp_add_ins (MINT_INTRINS_U32_TO_DECSTR);
			last_ins->data[0] = get_data_item_index (
				(char *) mono_vtable_get_static_field_data (vtable) + field->offset);
			last_ins->data[1] = get_data_item_index (
				mono_class_vtable_checked (rtm->domain, mono_defaults.string_class, error));
			sp--;
			interp_ins_set_sreg (last_ins, sp[0].local);
			push_type (StackType::O, mono_defaults.string_class);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 5;
			return TRUE;
		}
	} else if (in_corlib && !strcmp (klass_name_space, "System")
	           && (!strcmp (klass_name, "Math") || !strcmp (klass_name, "MathF"))) {
		gboolean is_float = strcmp (klass_name, "MathF") == 0;
		int param_type = is_float ? MONO_TYPE_R4 : MONO_TYPE_R8;
		// FIXME add also intrinsic for Round
		if (csignature->param_count == 1 && csignature->params[0]->type == param_type) {
			// unops
			if (tm[0] == 'A') {
				if (strcmp (tm, "Abs") == 0) {
					*op = MINT_ABS;
				} else if (strcmp (tm, "Asin") == 0) {
					*op = MINT_ASIN;
				} else if (strcmp (tm, "Asinh") == 0) {
					*op = MINT_ASINH;
				} else if (strcmp (tm, "Acos") == 0) {
					*op = MINT_ACOS;
				} else if (strcmp (tm, "Acosh") == 0) {
					*op = MINT_ACOSH;
				} else if (strcmp (tm, "Atan") == 0) {
					*op = MINT_ATAN;
				} else if (strcmp (tm, "Atanh") == 0) {
					*op = MINT_ATANH;
				}
			} else if (tm[0] == 'C') {
				if (strcmp (tm, "Ceiling") == 0) {
					*op = MINT_CEILING;
				} else if (strcmp (tm, "Cos") == 0) {
					*op = MINT_COS;
				} else if (strcmp (tm, "Cbrt") == 0) {
					*op = MINT_CBRT;
				} else if (strcmp (tm, "Cosh") == 0) {
					*op = MINT_COSH;
				}
			} else if (strcmp (tm, "Exp") == 0) {
				*op = MINT_EXP;
			} else if (strcmp (tm, "Floor") == 0) {
				*op = MINT_FLOOR;
			} else if (strcmp (tm, "ILogB") == 0) {
				*op = MINT_ILOGB;
			} else if (tm[0] == 'L') {
				if (strcmp (tm, "Log") == 0) {
					*op = MINT_LOG;
				} else if (strcmp (tm, "Log2") == 0) {
					*op = MINT_LOG2;
				} else if (strcmp (tm, "Log10") == 0) {
					*op = MINT_LOG10;
				}
			} else if (tm[0] == 'S') {
				if (strcmp (tm, "Sin") == 0) {
					*op = MINT_SIN;
				} else if (strcmp (tm, "Sqrt") == 0) {
					*op = MINT_SQRT;
				} else if (strcmp (tm, "Sinh") == 0) {
					*op = MINT_SINH;
				}
			} else if (tm[0] == 'T') {
				if (strcmp (tm, "Tan") == 0) {
					*op = MINT_TAN;
				} else if (strcmp (tm, "Tanh") == 0) {
					*op = MINT_TANH;
				}
			}
		} else if (csignature->param_count == 2 && csignature->params[0]->type == param_type
		           && csignature->params[1]->type == param_type) {
			if (strcmp (tm, "Atan2") == 0)
				*op = MINT_ATAN2;
			else if (strcmp (tm, "Pow") == 0)
				*op = MINT_POW;
		} else if (csignature->param_count == 3 && csignature->params[0]->type == param_type
		           && csignature->params[1]->type == param_type
		           && csignature->params[2]->type == param_type) {
			if (strcmp (tm, "FusedMultiplyAdd") == 0)
				*op = MINT_FMA;
		} else if (csignature->param_count == 2 && csignature->params[0]->type == param_type
		           && csignature->params[1]->type == MONO_TYPE_I4 && strcmp (tm, "ScaleB") == 0) {
			*op = MINT_SCALEB;
		}

		if (*op != -1 && is_float) {
			*op = *op + (MINT_ABSF - MINT_ABS);
		}
	} else if (in_corlib && !strcmp (klass_name_space, "System")
	           && (!strcmp (klass_name, "Span`1") || !strcmp (klass_name, "ReadOnlySpan`1"))) {
		if (!strcmp (tm, "get_Item")) {
			MonoGenericClass *gclass = mono_class_get_generic_class (target_method->klass);
			MonoClass *param_class =
				mono_class_from_mono_type_internal (gclass->context.class_inst->type_argv[0]);

			if (!mini_is_gsharedvt_variable_klass (param_class)) {
				MonoClassField *length_field =
					mono_class_get_field_from_name_full (target_method->klass, "_length", NULL);
				g_assert (length_field);
				int offset_length = length_field->offset - sizeof (MonoObject);

				MonoClassField *ptr_field =
					mono_class_get_field_from_name_full (target_method->klass, "_pointer", NULL);
				g_assert (ptr_field);
				int offset_pointer = ptr_field->offset - sizeof (MonoObject);

				int size = mono_class_array_element_size (param_class);
				interp_add_ins (MINT_GETITEM_SPAN);
				last_ins->data[0] = size;
				last_ins->data[1] = offset_length;
				last_ins->data[2] = offset_pointer;

				sp -= 2;
				interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
				push_simple_type (StackType::MP);
				interp_ins_set_dreg (last_ins, sp[-1].local);
				ip += 5;
				return TRUE;
			}
		} else if (!strcmp (tm, "get_Length")) {
			MonoClassField *length_field =
				mono_class_get_field_from_name_full (target_method->klass, "_length", NULL);
			g_assert (length_field);
			int offset_length = length_field->offset - sizeof (MonoObject);
			interp_add_ins (MINT_LDLEN_SPAN);
			last_ins->data[0] = offset_length;
			sp--;
			interp_ins_set_sreg (last_ins, sp[0].local);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp[-1].local);
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
			interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
			push_type (stack_type_of (mint_type (m_class_get_byval_arg (k))), k);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 5;
			return TRUE;
		} else if (!strcmp (tm, "SizeOf")) {
			MonoGenericContext *ctx = mono_method_get_context (target_method);
			g_assert (ctx);
			g_assert (ctx->method_inst);
			g_assert (ctx->method_inst->type_argc == 1);
			MonoType *t = ctx->method_inst->type_argv[0];
			int align;
			int esize = mono_type_size (t, &align);
			interp_add_ins (MINT_LDC_I4);
			WRITE32_INS (last_ins, 0, &esize);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp[-1].local);
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
	} else if (in_corlib && !strcmp (klass_name_space, "System.Runtime.CompilerServices")
	           && !strcmp (klass_name, "RuntimeHelpers")) {
#ifdef ENABLE_NETCORE
		if (!strcmp (tm, "get_OffsetToStringData")) {
			g_assert (csignature->param_count == 0);
			int offset = MONO_STRUCT_OFFSET (MonoString, chars);
			interp_add_ins (MINT_LDC_I4);
			WRITE32_INS (last_ins, 0, &offset);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 5;
			return TRUE;
		} else if (!strcmp (tm, "GetRawData")) {
			interp_add_ins (MINT_LDFLDA_UNSAFE);
			last_ins->data[0] = (gint16) MONO_ABI_SIZEOF (MonoObject);

			sp--;
			interp_ins_set_sreg (last_ins, sp[0].local);
			push_simple_type (StackType::MP);
			interp_ins_set_dreg (last_ins, sp[-1].local);

			ip += 5;
			return TRUE;
		} else if (!strcmp (tm, "IsBitwiseEquatable")) {
			g_assert (csignature->param_count == 0);
			MonoGenericContext *ctx = mono_method_get_context (target_method);
			g_assert (ctx);
			g_assert (ctx->method_inst);
			g_assert (ctx->method_inst->type_argc == 1);
			MonoType *t = mini_get_underlying_type (ctx->method_inst->type_argv[0]);

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
			MonoType *t = mini_get_underlying_type (ctx->method_inst->type_argv[0]);

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
	} else if (in_corlib && !strcmp (klass_name_space, "System")
	           && !strcmp (klass_name, "RuntimeMethodHandle") && !strcmp (tm, "GetFunctionPointer")
	           && csignature->param_count == 1) {
		// Same answer as the icall, without transforming the method to get it.
		*op = MINT_LDFTN_DYNAMIC;
	} else if (in_corlib && target_method->klass == mono_defaults.systemtype_class
	           && !strcmp (target_method->name, "op_Equality")) {
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
	} else if (in_corlib && target_method->klass == mono_defaults.enum_class
	           && !strcmp (tm, "HasFlag")) {
		gboolean intrinsify = FALSE;
		MonoClass *base_klass = NULL;
		if (last_ins && last_ins->opcode == MINT_BOX && last_ins->prev
		    && interp_ins_is_ldc (last_ins->prev) && last_ins->prev->prev
		    && last_ins->prev->prev->opcode == MINT_BOX && sp[-2].klass == sp[-1].klass
		    && interp_ip_in_cbb (ip - il_code)) {
			// csc pattern : box, ldc, box, call HasFlag
			g_assert (m_class_is_enumtype (sp[-2].klass));
			MonoType *base_type =
				mono_type_get_underlying_type (m_class_get_byval_arg (sp[-2].klass));
			base_klass = mono_class_from_mono_type_internal (base_type);

			// Remove the boxing of valuetypes, by replacing them with moves
			last_ins->prev->prev->opcode = get_mov_for_type (mint_type (base_type), FALSE);
			last_ins->opcode = get_mov_for_type (mint_type (base_type), FALSE);

			intrinsify = TRUE;
		} else if (last_ins && last_ins->opcode == MINT_BOX && last_ins->prev
		           && interp_ins_is_ldc (last_ins->prev) && constrained_class
		           && sp[-1].klass == constrained_class && interp_ip_in_cbb (ip - il_code)) {
			// mcs pattern : ldc, box, constrained Enum, call HasFlag
			g_assert (m_class_is_enumtype (constrained_class));
			MonoType *base_type =
				mono_type_get_underlying_type (m_class_get_byval_arg (constrained_class));
			base_klass = mono_class_from_mono_type_internal (base_type);
			MintType mt = mint_type (m_class_get_byval_arg (base_klass));

			// Remove boxing and load the value of this
			last_ins->opcode = get_mov_for_type (mt, FALSE);
			InterpInst *ins =
				interp_insert_ins (last_ins->prev->prev, interp_get_ldind_for_mt (mt));
			interp_ins_set_sreg (ins, sp[-2].local);
			interp_ins_set_dreg (ins, sp[-2].local);
			intrinsify = TRUE;
		}
		if (intrinsify) {
			interp_add_ins (MINT_INTRINS_ENUM_HASFLAG);
			last_ins->data[0] = get_data_item_index (base_klass);
			sp -= 2;
			interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			ip += 5;
			return TRUE;
		}
	} else if (in_corlib && !strcmp (klass_name_space, "System.Threading")
	           && !strcmp (klass_name, "Interlocked")) {
		if (!strcmp (tm, "MemoryBarrier") && csignature->param_count == 0)
			*op = MINT_MONO_MEMORY_BARRIER;
		else if (!strcmp (tm, "Exchange") && csignature->param_count == 2
		         && csignature->params[0]->type == MONO_TYPE_I8
		         && csignature->params[1]->type == MONO_TYPE_I8)
			*op = MINT_MONO_EXCHANGE_I8;
	} else if (in_corlib && !strcmp (klass_name_space, "System.Threading")
	           && !strcmp (klass_name, "Thread")) {
		if (!strcmp (tm, "MemoryBarrier") && csignature->param_count == 0)
			*op = MINT_MONO_MEMORY_BARRIER;
	} else if (in_corlib && !strcmp (klass_name_space, "System.Runtime.CompilerServices")
	           && !strcmp (klass_name, "JitHelpers")
	           && (!strcmp (tm, "EnumEquals") || !strcmp (tm, "EnumCompareTo"))) {
		MonoGenericContext *ctx = mono_method_get_context (target_method);
		g_assert (ctx);
		g_assert (ctx->method_inst);
		g_assert (ctx->method_inst->type_argc == 1);
		g_assert (csignature->param_count == 2);

		MonoType *t = ctx->method_inst->type_argv[0];
		t = mini_get_underlying_type (t);

		gboolean is_i8 = (t->type == MONO_TYPE_I8 || t->type == MONO_TYPE_U8);
		gboolean is_unsigned =
			(t->type == MONO_TYPE_U1 || t->type == MONO_TYPE_U2 || t->type == MONO_TYPE_U4
		     || t->type == MONO_TYPE_U8 || t->type == MONO_TYPE_U);

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
			interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			// (a < b)
			load_local (locala);
			load_local (localb);
			if (is_unsigned)
				interp_add_ins (is_i8 ? MINT_CLT_UN_I8 : MINT_CLT_UN_I4);
			else
				interp_add_ins (is_i8 ? MINT_CLT_I8 : MINT_CLT_I4);
			sp -= 2;
			interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp[-1].local);
			// (a > b) - (a < b)
			interp_add_ins (MINT_SUB_I4);
			sp -= 2;
			interp_ins_set_sregs2 (last_ins, sp[0].local, sp[1].local);
			push_simple_type (StackType::I4);
			interp_ins_set_dreg (last_ins, sp[-1].local);
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
	else if (in_corlib && !strcmp ("System.Runtime.CompilerServices", klass_name_space)
	         && !strcmp ("RuntimeFeature", klass_name)) {
		if (!strcmp (tm, "get_IsDynamicCodeSupported"))
			*op = MINT_LDC_I4_1;
		else if (!strcmp (tm, "get_IsDynamicCodeCompiled"))
			*op = MINT_LDC_I4_0;
	} else if (in_corlib && !strncmp ("System.Runtime.Intrinsics", klass_name_space, 25)
	           && !strcmp (tm, "get_IsSupported")) {
		*op = MINT_LDC_I4_0;
	}
#endif

	return FALSE;
}

MonoMethod *
interp_transform_internal_calls (MonoMethod *method, MonoMethod *target_method,
                                 MonoMethodSignature *csignature, gboolean is_virtual)
{
	if (((method->wrapper_type == MONO_WRAPPER_NONE)
	     || (method->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD))
	    && target_method != NULL) {
		if (target_method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)
			target_method = mono_marshal_get_native_wrapper (target_method, FALSE, FALSE);
		if (!is_virtual && target_method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
			target_method = mono_marshal_get_synchronized_wrapper (target_method);

		if (target_method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL && !is_virtual
		    && !mono_class_is_marshalbyref (target_method->klass)
		    && m_class_get_rank (target_method->klass) == 0)
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

int
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
			if (INTERP_TYPE_AS_PTR (sig->params[0]))
				op = MINT_ICALL_P_V;
		} else if (INTERP_TYPE_AS_PTR (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params[0]))
				op = MINT_ICALL_P_P;
		}
		break;
	case 2:
		if (MONO_TYPE_IS_VOID (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params[0]) && INTERP_TYPE_AS_PTR (sig->params[1]))
				op = MINT_ICALL_PP_V;
		} else if (INTERP_TYPE_AS_PTR (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params[0]) && INTERP_TYPE_AS_PTR (sig->params[1]))
				op = MINT_ICALL_PP_P;
		}
		break;
	case 3:
		if (MONO_TYPE_IS_VOID (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params[0]) && INTERP_TYPE_AS_PTR (sig->params[1])
			    && INTERP_TYPE_AS_PTR (sig->params[2]))
				op = MINT_ICALL_PPP_V;
		} else if (INTERP_TYPE_AS_PTR (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params[0]) && INTERP_TYPE_AS_PTR (sig->params[1])
			    && INTERP_TYPE_AS_PTR (sig->params[2]))
				op = MINT_ICALL_PPP_P;
		}
		break;
	case 4:
		if (MONO_TYPE_IS_VOID (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params[0]) && INTERP_TYPE_AS_PTR (sig->params[1])
			    && INTERP_TYPE_AS_PTR (sig->params[2]) && INTERP_TYPE_AS_PTR (sig->params[3]))
				op = MINT_ICALL_PPPP_V;
		} else if (INTERP_TYPE_AS_PTR (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params[0]) && INTERP_TYPE_AS_PTR (sig->params[1])
			    && INTERP_TYPE_AS_PTR (sig->params[2]) && INTERP_TYPE_AS_PTR (sig->params[3]))
				op = MINT_ICALL_PPPP_P;
		}
		break;
	case 5:
		if (MONO_TYPE_IS_VOID (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params[0]) && INTERP_TYPE_AS_PTR (sig->params[1])
			    && INTERP_TYPE_AS_PTR (sig->params[2]) && INTERP_TYPE_AS_PTR (sig->params[3])
			    && INTERP_TYPE_AS_PTR (sig->params[4]))
				op = MINT_ICALL_PPPPP_V;
		} else if (INTERP_TYPE_AS_PTR (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params[0]) && INTERP_TYPE_AS_PTR (sig->params[1])
			    && INTERP_TYPE_AS_PTR (sig->params[2]) && INTERP_TYPE_AS_PTR (sig->params[3])
			    && INTERP_TYPE_AS_PTR (sig->params[4]))
				op = MINT_ICALL_PPPPP_P;
		}
		break;
	case 6:
		if (MONO_TYPE_IS_VOID (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params[0]) && INTERP_TYPE_AS_PTR (sig->params[1])
			    && INTERP_TYPE_AS_PTR (sig->params[2]) && INTERP_TYPE_AS_PTR (sig->params[3])
			    && INTERP_TYPE_AS_PTR (sig->params[4]) && INTERP_TYPE_AS_PTR (sig->params[5]))
				op = MINT_ICALL_PPPPPP_V;
		} else if (INTERP_TYPE_AS_PTR (sig->ret)) {
			if (INTERP_TYPE_AS_PTR (sig->params[0]) && INTERP_TYPE_AS_PTR (sig->params[1])
			    && INTERP_TYPE_AS_PTR (sig->params[2]) && INTERP_TYPE_AS_PTR (sig->params[3])
			    && INTERP_TYPE_AS_PTR (sig->params[4]) && INTERP_TYPE_AS_PTR (sig->params[5]))
				op = MINT_ICALL_PPPPPP_P;
		}
		break;
	}
	return op;
}

} // namespace mono::interp
