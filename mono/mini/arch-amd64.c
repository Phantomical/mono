/**
 * \file
 * AMD64 architecture support: the calling convention, the hand-emitted stubs
 * the runtime dispatches through, and the arch hooks the debugger, the
 * unwinder and the trampolines ask for.
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

#include "mini-amd64.h"
#include "mini-runtime.h"
#include "debugger-agent.h"
#include "aot-runtime.h"

#ifdef MONO_XEN_OPT
gboolean mono_amd64_optimize_for_xen = TRUE;
#endif

#define IS_REX(inst) (((inst) >= 0x40) && ((inst) <= 0x4f))

/* The single step trampoline */
gpointer mono_amd64_ss_trampoline;

/* The breakpoint trampoline */
gpointer mono_amd64_bp_trampoline;


const char*
mono_arch_regname (int reg)
{
	switch (reg) {
	case AMD64_RAX: return "%rax";
	case AMD64_RBX: return "%rbx";
	case AMD64_RCX: return "%rcx";
	case AMD64_RDX: return "%rdx";
	case AMD64_RSP: return "%rsp";	
	case AMD64_RBP: return "%rbp";
	case AMD64_RDI: return "%rdi";
	case AMD64_RSI: return "%rsi";
	case AMD64_R8: return "%r8";
	case AMD64_R9: return "%r9";
	case AMD64_R10: return "%r10";
	case AMD64_R11: return "%r11";
	case AMD64_R12: return "%r12";
	case AMD64_R13: return "%r13";
	case AMD64_R14: return "%r14";
	case AMD64_R15: return "%r15";
	}
	return "unknown";
}

static const char * const packed_xmmregs [] = {
	"p:xmm0", "p:xmm1", "p:xmm2", "p:xmm3", "p:xmm4", "p:xmm5", "p:xmm6", "p:xmm7", "p:xmm8",
	"p:xmm9", "p:xmm10", "p:xmm11", "p:xmm12", "p:xmm13", "p:xmm14", "p:xmm15"
};

static const char * const single_xmmregs [] = {
	"s:xmm0", "s:xmm1", "s:xmm2", "s:xmm3", "s:xmm4", "s:xmm5", "s:xmm6", "s:xmm7", "s:xmm8",
	"s:xmm9", "s:xmm10", "s:xmm11", "s:xmm12", "s:xmm13", "s:xmm14", "s:xmm15"
};

const char*
mono_arch_fregname (int reg)
{
	if (reg < AMD64_XMM_NREG)
		return single_xmmregs [reg];
	else
		return "unknown";
}

const char *
mono_arch_xregname (int reg)
{
	if (reg < AMD64_XMM_NREG)
		return packed_xmmregs [reg];
	else
		return "unknown";
}

void
mono_x86_patch (unsigned char* code, gpointer target)
{
	mono_x86_patch_inline (code, target);
}

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

void
mono_amd64_patch (unsigned char* code, gpointer target)
{
	amd64_patch (code, target);
}

gboolean
mono_is_regsize_var (MonoType *t)
{
	t = mini_get_underlying_type (t);
	switch (t->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
#if SIZEOF_REGISTER == 8
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
#endif
		return TRUE;
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_ARRAY:
		return TRUE;
	case MONO_TYPE_GENERICINST:
		if (!mono_type_generic_inst_is_valuetype (t))
			return TRUE;
		return FALSE;
	case MONO_TYPE_VALUETYPE:
		return FALSE;
	default:
		return FALSE;
	}
}

static gboolean
amd64_is_near_call (guint8 *code)
{
	/* Skip REX */
	if ((code [0] >= 0x40) && (code [0] <= 0x4f))
		code += 1;

	return code [0] == 0xe8;
}

void
mono_arch_patch_code_new (MonoCompile *cfg, MonoDomain *domain, guint8 *code, MonoJumpInfo *ji, gpointer target)
{
	unsigned char *ip = ji->ip.i + code;

	/*
	 * Debug code to help track down problems where the target of a near call is
	 * is not valid.
	 */
	if (amd64_is_near_call (ip)) {
		gint64 disp = (guint8*)target - (guint8*)ip;

		if (!amd64_is_imm32 (disp)) {
			printf ("TYPE: %d\n", ji->type);
			switch (ji->type) {
			case MONO_PATCH_INFO_JIT_ICALL_ID:
				printf ("V: %s\n", mono_find_jit_icall_info (ji->data.jit_icall_id)->name);
				break;
			case MONO_PATCH_INFO_METHOD_JUMP:
			case MONO_PATCH_INFO_METHOD:
				printf ("V: %s\n", ji->data.method->name);
				break;
			default:
				break;
			}
		}
	}

	amd64_patch (ip, (gpointer)target);
}

/*
 * mono_arch_emit_load_aotconst:
 *
 *   Emit code to load the contents of the GOT slot identified by TRAMP_TYPE and
 * TARGET from the mscorlib GOT in full-aot code.
 * On AMD64, the result is placed into R11.
 */
guint8*
mono_arch_emit_load_aotconst (guint8 *start, guint8 *code, MonoJumpInfo **ji, MonoJumpInfoType tramp_type, gconstpointer target)
{
	*ji = mono_patch_info_list_prepend (*ji, code - start, tramp_type, target);
	amd64_mov_reg_membase (code, AMD64_R11, AMD64_RIP, 0, 8);

	return code;
}

static void inline
add_general (guint32 *gr, guint32 *stack_size, ArgInfo *ainfo)
{
    ainfo->offset = *stack_size;

    if (*gr >= PARAM_REGS) {
		ainfo->storage = ArgOnStack;
		ainfo->arg_size = sizeof (target_mgreg_t);
		/* Since the same stack slot size is used for all arg */
		/*  types, it needs to be big enough to hold them all */
		(*stack_size) += sizeof (target_mgreg_t);
    }
    else {
		ainfo->storage = ArgInIReg;
		ainfo->reg = param_regs [*gr];
		(*gr) ++;
    }
}

static void inline
add_float (guint32 *gr, guint32 *stack_size, ArgInfo *ainfo, gboolean is_double)
{
    ainfo->offset = *stack_size;

    if (*gr >= FLOAT_PARAM_REGS) {
		ainfo->storage = ArgOnStack;
		ainfo->arg_size = sizeof (target_mgreg_t);
		/* Since the same stack slot size is used for both float */
		/*  types, it needs to be big enough to hold them both */
		(*stack_size) += sizeof (target_mgreg_t);
    }
    else {
		/* A double register */
		if (is_double)
			ainfo->storage = ArgInDoubleSSEReg;
		else
			ainfo->storage = ArgInFloatSSEReg;
		ainfo->reg = *gr;
		(*gr) += 1;
    }
}

typedef enum ArgumentClass {
	ARG_CLASS_NO_CLASS,
	ARG_CLASS_MEMORY,
	ARG_CLASS_INTEGER,
	ARG_CLASS_SSE
} ArgumentClass;

static ArgumentClass
merge_argument_class_from_type (MonoType *type, ArgumentClass class1)
{
	ArgumentClass class2 = ARG_CLASS_NO_CLASS;
	MonoType *ptype;

	ptype = mini_get_underlying_type (type);
	switch (ptype->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		class2 = ARG_CLASS_INTEGER;
		break;
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
#ifdef TARGET_WIN32
		class2 = ARG_CLASS_INTEGER;
#else
		class2 = ARG_CLASS_SSE;
#endif
		break;

	case MONO_TYPE_TYPEDBYREF:
		g_assert_not_reached ();

	case MONO_TYPE_GENERICINST:
		if (!mono_type_generic_inst_is_valuetype (ptype)) {
			class2 = ARG_CLASS_INTEGER;
			break;
		}
		/* fall through */
	case MONO_TYPE_VALUETYPE: {
		MonoMarshalType *info = mono_marshal_load_type_info (ptype->data.klass);
		int i;

		for (i = 0; i < info->num_fields; ++i) {
			class2 = class1;
			class2 = merge_argument_class_from_type (info->fields [i].field->type, class2);
		}
		break;
	}
	default:
		g_assert_not_reached ();
	}

	/* Merge */
	if (class1 == class2)
		;
	else if (class1 == ARG_CLASS_NO_CLASS)
		class1 = class2;
	else if ((class1 == ARG_CLASS_MEMORY) || (class2 == ARG_CLASS_MEMORY))
		class1 = ARG_CLASS_MEMORY;
	else if ((class1 == ARG_CLASS_INTEGER) || (class2 == ARG_CLASS_INTEGER))
		class1 = ARG_CLASS_INTEGER;
	else
		class1 = ARG_CLASS_SSE;

	return class1;
}

typedef struct {
	MonoType *type;
	int size, offset;
} StructFieldInfo;

/*
 * collect_field_info_nested:
 *
 *   Collect field info from KLASS recursively into FIELDS.
 */
static void
collect_field_info_nested (MonoClass *klass, GArray *fields_array, int offset, gboolean pinvoke, gboolean unicode)
{
	MonoMarshalType *info;
	int i;

	if (pinvoke) {
		info = mono_marshal_load_type_info (klass);
		g_assert(info);
		for (i = 0; i < info->num_fields; ++i) {
			if (MONO_TYPE_ISSTRUCT (info->fields [i].field->type)) {
				collect_field_info_nested (mono_class_from_mono_type_internal (info->fields [i].field->type), fields_array, info->fields [i].offset, pinvoke, unicode);
			} else {
				guint32 align;
				StructFieldInfo f;

				f.type = info->fields [i].field->type;
				f.size = mono_marshal_type_size (info->fields [i].field->type,
															   info->fields [i].mspec,
															   &align, TRUE, unicode);
				f.offset = offset + info->fields [i].offset;
				if (i == info->num_fields - 1 && f.size + f.offset < info->native_size) {
					/* This can happen with .pack directives eg. 'fixed' arrays */
					if (MONO_TYPE_IS_PRIMITIVE (f.type)) {
						/* Replicate the last field to fill out the remaining place, since the code in add_valuetype () needs type information */
						g_array_append_val (fields_array, f);
						while (f.size + f.offset < info->native_size) {
							f.offset += f.size;
							g_array_append_val (fields_array, f);
						}
					} else {
						f.size = info->native_size - f.offset;
						g_array_append_val (fields_array, f);
					}
				} else {
					g_array_append_val (fields_array, f);
				}
			}
		}
	} else {
		gpointer iter;
		MonoClassField *field;

		iter = NULL;
		while ((field = mono_class_get_fields_internal (klass, &iter))) {
			if (field->type->attrs & FIELD_ATTRIBUTE_STATIC)
				continue;
			if (MONO_TYPE_ISSTRUCT (field->type)) {
				collect_field_info_nested (mono_class_from_mono_type_internal (field->type), fields_array, field->offset - MONO_ABI_SIZEOF (MonoObject), pinvoke, unicode);
			} else {
				int align;
				StructFieldInfo f;

				f.type = field->type;
				f.size = mono_type_size (field->type, &align);
				f.offset = field->offset - MONO_ABI_SIZEOF (MonoObject) + offset;

				g_array_append_val (fields_array, f);
			}
		}
	}
}

#ifdef TARGET_WIN32

/* Windows x64 ABI can pass/return value types in register of size 1,2,4,8 bytes. */
#define MONO_WIN64_VALUE_TYPE_FITS_REG(arg_size) (arg_size <= SIZEOF_REGISTER && (arg_size == 1 || arg_size == 2 || arg_size == 4 || arg_size == 8))

static gboolean
allocate_register_for_valuetype_win64 (ArgInfo *arg_info, ArgumentClass arg_class, guint32 arg_size, const AMD64_Reg_No int_regs [], int int_reg_count, const AMD64_XMM_Reg_No float_regs [], int float_reg_count, guint32 *current_int_reg, guint32 *current_float_reg)
{
	gboolean result = FALSE;

	assert (arg_info != NULL && int_regs != NULL && float_regs != NULL && current_int_reg != NULL && current_float_reg != NULL);
	assert (arg_info->storage == ArgValuetypeInReg || arg_info->storage == ArgValuetypeAddrInIReg);

	arg_info->pair_storage [0] = arg_info->pair_storage [1] = ArgNone;
	arg_info->pair_regs [0] = arg_info->pair_regs [1] = ArgNone;
	arg_info->pair_size [0] = 0;
	arg_info->pair_size [1] = 0;
	arg_info->nregs = 0;

	if (arg_class == ARG_CLASS_INTEGER && *current_int_reg < int_reg_count) {
		/* Pass parameter in integer register. */
		arg_info->pair_storage [0] = ArgInIReg;
		arg_info->pair_regs [0] = int_regs [*current_int_reg];
		(*current_int_reg) ++;
		result = TRUE;
	} else if (arg_class == ARG_CLASS_SSE && *current_float_reg < float_reg_count) {
		/* Pass parameter in float register. */
		arg_info->pair_storage [0] = (arg_size <= sizeof (gfloat)) ? ArgInFloatSSEReg : ArgInDoubleSSEReg;
		arg_info->pair_regs [0] = float_regs [*current_float_reg];
		(*current_float_reg) ++;
		result = TRUE;
	}

	if (result == TRUE) {
		arg_info->pair_size [0] = arg_size;
		arg_info->nregs = 1;
	}

	return result;
}

static gboolean
allocate_parameter_register_for_valuetype_win64 (ArgInfo *arg_info, ArgumentClass arg_class, guint32 arg_size, guint32 *current_int_reg, guint32 *current_float_reg)
{
	return allocate_register_for_valuetype_win64 (arg_info, arg_class, arg_size, param_regs, PARAM_REGS, float_param_regs, FLOAT_PARAM_REGS, current_int_reg, current_float_reg);
}

static gboolean
allocate_return_register_for_valuetype_win64 (ArgInfo *arg_info, ArgumentClass arg_class, guint32 arg_size, guint32 *current_int_reg, guint32 *current_float_reg)
{
	return allocate_register_for_valuetype_win64 (arg_info, arg_class, arg_size, return_regs, RETURN_REGS, float_return_regs, FLOAT_RETURN_REGS, current_int_reg, current_float_reg);
}

static void
allocate_storage_for_valuetype_win64 (ArgInfo *arg_info, MonoType *type, gboolean is_return, ArgumentClass arg_class,
									  guint32 arg_size, guint32 *current_int_reg, guint32 *current_float_reg, guint32 *stack_size)
{
	/* Windows x64 value type ABI.
	*
	* Parameters: https://msdn.microsoft.com/en-us/library/zthk2dkh.aspx
	*
	* Integer/Float types smaller than or equals to 8 bytes or porperly sized struct/union (1,2,4,8)
	*    Try pass in register using ArgValuetypeInReg/(ArgInIReg|ArgInFloatSSEReg|ArgInDoubleSSEReg) as storage and size of parameter(1,2,4,8), if no more registers, pass on stack using ArgOnStack as storage and size of parameter(1,2,4,8).
	* Integer/Float types bigger than 8 bytes or struct/unions larger than 8 bytes or (3,5,6,7).
	*    Try to pass pointer in register using ArgValuetypeAddrInIReg, if no more registers, pass pointer on stack using ArgValuetypeAddrOnStack as storage and parameter size of register (8 bytes).
	*
	* Return values:  https://msdn.microsoft.com/en-us/library/7572ztz4.aspx.
	*
	* Integers/Float types smaller than or equal to 8 bytes
	*    Return in corresponding register RAX/XMM0 using ArgValuetypeInReg/(ArgInIReg|ArgInFloatSSEReg|ArgInDoubleSSEReg) as storage and size of parameter(1,2,4,8).
	* Properly sized struct/unions (1,2,4,8)
	*    Return in register RAX using ArgValuetypeInReg as storage and size of parameter(1,2,4,8).
	* Types bigger than 8 bytes or struct/unions larger than 8 bytes or (3,5,6,7).
	*    Return pointer to allocated stack space (allocated by caller) using ArgValuetypeAddrInIReg as storage and parameter size.
	*/

	assert (arg_info != NULL && type != NULL && current_int_reg != NULL && current_float_reg != NULL && stack_size != NULL);

	if (!is_return) {

		/* Parameter cases. */
		if (arg_class != ARG_CLASS_MEMORY && MONO_WIN64_VALUE_TYPE_FITS_REG (arg_size)) {
			assert (arg_size == 1 || arg_size == 2 || arg_size == 4 || arg_size == 8);

			/* First, try to use registers for parameter. If type is struct it can only be passed by value in integer register. */
			arg_info->storage = ArgValuetypeInReg;
			if (!allocate_parameter_register_for_valuetype_win64 (arg_info, !MONO_TYPE_ISSTRUCT (type) ? arg_class : ARG_CLASS_INTEGER, arg_size, current_int_reg, current_float_reg)) {
				/* No more registers, fallback passing parameter on stack as value. */
				assert (arg_info->pair_storage [0] == ArgNone && arg_info->pair_storage [1] == ArgNone && arg_info->pair_size [0] == 0 && arg_info->pair_size [1] == 0 && arg_info->nregs == 0);
				
				/* Passing value directly on stack, so use size of value. */
				arg_info->storage = ArgOnStack;
				arg_size = ALIGN_TO (arg_size, sizeof (target_mgreg_t));
				arg_info->offset = *stack_size;
				arg_info->arg_size = arg_size;
				*stack_size += arg_size;
			}
		} else {
			/* Fallback to stack, try to pass address to parameter in register. Always use integer register to represent stack address. */
			arg_info->storage = ArgValuetypeAddrInIReg;
			if (!allocate_parameter_register_for_valuetype_win64 (arg_info, ARG_CLASS_INTEGER, arg_size, current_int_reg, current_float_reg)) {
				/* No more registers, fallback passing address to parameter on stack. */
				assert (arg_info->pair_storage [0] == ArgNone && arg_info->pair_storage [1] == ArgNone && arg_info->pair_size [0] == 0 && arg_info->pair_size [1] == 0 && arg_info->nregs == 0);
								
				/* Passing an address to value on stack, so use size of register as argument size. */
				arg_info->storage = ArgValuetypeAddrOnStack;
				arg_size = sizeof (target_mgreg_t);
				arg_info->offset = *stack_size;
				arg_info->arg_size = arg_size;
				*stack_size += arg_size;
			}
		}
	} else {
		/* Return value cases. */
		if (arg_class != ARG_CLASS_MEMORY && MONO_WIN64_VALUE_TYPE_FITS_REG (arg_size)) {
			assert (arg_size == 1 || arg_size == 2 || arg_size == 4 || arg_size == 8);

			/* Return value fits into return registers. If type is struct it can only be returned by value in integer register. */
			arg_info->storage = ArgValuetypeInReg;
			allocate_return_register_for_valuetype_win64 (arg_info, !MONO_TYPE_ISSTRUCT (type) ? arg_class : ARG_CLASS_INTEGER, arg_size, current_int_reg, current_float_reg);

			/* Only RAX/XMM0 should be used to return valuetype. */
			assert ((arg_info->pair_regs[0] == AMD64_RAX && arg_info->pair_regs[1] == ArgNone) || (arg_info->pair_regs[0] == AMD64_XMM0 && arg_info->pair_regs[1] == ArgNone));
		} else {
			/* Return value doesn't fit into return register, return address to allocated stack space (allocated by caller and passed as input). */
			arg_info->storage = ArgValuetypeAddrInIReg;
			allocate_return_register_for_valuetype_win64 (arg_info, ARG_CLASS_INTEGER, arg_size, current_int_reg, current_float_reg);

			/* Only RAX should be used to return valuetype address. */
			assert (arg_info->pair_regs[0] == AMD64_RAX && arg_info->pair_regs[1] == ArgNone);

			arg_size = ALIGN_TO (arg_size, sizeof (target_mgreg_t));
			arg_info->offset = *stack_size;
			*stack_size += arg_size;
		}
	}
}

static void
get_valuetype_size_win64 (MonoClass *klass, gboolean pinvoke, ArgInfo *arg_info, MonoType *type, ArgumentClass *arg_class, guint32 *arg_size)
{
	*arg_size = 0;
	*arg_class = ARG_CLASS_NO_CLASS;

	assert (klass != NULL && arg_info != NULL && type != NULL && arg_class != NULL && arg_size != NULL);
	
	if (pinvoke) {
		/* Calculate argument class type and size of marshalled type. */
		MonoMarshalType *info = mono_marshal_load_type_info (klass);
		*arg_size = info->native_size;
	} else {
		/* Calculate argument class type and size of managed type. */
		*arg_size = mono_class_value_size (klass, NULL);
	}

	/* Windows ABI only handle value types on stack or passed in integer register (if it fits register size). */
	*arg_class = MONO_WIN64_VALUE_TYPE_FITS_REG (*arg_size) ? ARG_CLASS_INTEGER : ARG_CLASS_MEMORY;

	if (*arg_class == ARG_CLASS_MEMORY) {
		/* Value type has a size that doesn't seem to fit register according to ABI. Try to used full stack size of type. */
		*arg_size = mini_type_stack_size_full (m_class_get_byval_arg (klass), NULL, pinvoke);
	}

	/*
	* Standard C and C++ doesn't allow empty structs, empty structs will always have a size of 1 byte.
	* GCC have an extension to allow empty structs, https://gcc.gnu.org/onlinedocs/gcc/Empty-Structures.html.
	* This cause a little dilemma since runtime build using none GCC compiler will not be compatible with
	* GCC build C libraries and the other way around. On platforms where empty structs has size of 1 byte
	* it must be represented in call and cannot be dropped.
	*/
	if (*arg_size == 0 && MONO_TYPE_ISSTRUCT (type)) {
		arg_info->pass_empty_struct = TRUE;
		*arg_size = SIZEOF_REGISTER;
		*arg_class = ARG_CLASS_INTEGER;
	}

	assert (*arg_class != ARG_CLASS_NO_CLASS);
}

static void
add_valuetype_win64 (MonoMethodSignature *signature, ArgInfo *arg_info, MonoType *type,
						gboolean is_return, guint32 *current_int_reg, guint32 *current_float_reg, guint32 *stack_size)
{
	guint32 arg_size = SIZEOF_REGISTER;
	MonoClass *klass = NULL;
	ArgumentClass arg_class;
	
	assert (signature != NULL && arg_info != NULL && type != NULL && current_int_reg != NULL && current_float_reg != NULL && stack_size != NULL);

	klass = mono_class_from_mono_type_internal (type);
	get_valuetype_size_win64 (klass, signature->pinvoke, arg_info, type, &arg_class, &arg_size);

	/* Only drop value type if its not an empty struct as input that must be represented in call */
	if ((arg_size == 0 && !arg_info->pass_empty_struct) || (arg_info->pass_empty_struct && is_return)) {
		arg_info->storage = ArgValuetypeInReg;
		arg_info->pair_storage [0] = arg_info->pair_storage [1] = ArgNone;
	} else {
		/* Alocate storage for value type. */
		allocate_storage_for_valuetype_win64 (arg_info, type, is_return, arg_class, arg_size, current_int_reg, current_float_reg, stack_size);
	}
}

#endif /* TARGET_WIN32 */

static void
add_valuetype (MonoMethodSignature *sig, ArgInfo *ainfo, MonoType *type,
			   gboolean is_return,
			   guint32 *gr, guint32 *fr, guint32 *stack_size)
{
#ifdef TARGET_WIN32
	add_valuetype_win64 (sig, ainfo, type, is_return, gr, fr, stack_size);
#else
	guint32 size, quad, nquads, i, nfields;
	/* Keep track of the size used in each quad so we can */
	/* use the right size when copying args/return vars.  */
	guint32 quadsize [2] = {8, 8};
	ArgumentClass args [2];
	StructFieldInfo *fields = NULL;
	GArray *fields_array;
	MonoClass *klass;
	gboolean pass_on_stack = FALSE;
	int struct_size;

	klass = mono_class_from_mono_type_internal (type);
	size = mini_type_stack_size_full (m_class_get_byval_arg (klass), NULL, sig->pinvoke);

	if (!sig->pinvoke && ((is_return && (size == 8)) || (!is_return && (size <= 16)))) {
		/* We pass and return vtypes of size 8 in a register */
	} else if (!sig->pinvoke || (size == 0) || (size > 16)) {
		pass_on_stack = TRUE;
	}

	/* If this struct can't be split up naturally into 8-byte */
	/* chunks (registers), pass it on the stack.              */
	if (sig->pinvoke) {
		MonoMarshalType *info = mono_marshal_load_type_info (klass);
		g_assert (info);
		struct_size = info->native_size;
	} else {
		struct_size = mono_class_value_size (klass, NULL);
	}
	/*
	 * Collect field information recursively to be able to
	 * handle nested structures.
	 */
	fields_array = g_array_new (FALSE, TRUE, sizeof (StructFieldInfo));
	collect_field_info_nested (klass, fields_array, 0, sig->pinvoke, m_class_is_unicode (klass));
	fields = (StructFieldInfo*)fields_array->data;
	nfields = fields_array->len;

	for (i = 0; i < nfields; ++i) {
		if ((fields [i].offset < 8) && (fields [i].offset + fields [i].size) > 8) {
			pass_on_stack = TRUE;
			break;
		}
	}

	if (size == 0) {
		ainfo->storage = ArgValuetypeInReg;
		ainfo->pair_storage [0] = ainfo->pair_storage [1] = ArgNone;
		return;
	}

	if (pass_on_stack) {
		/* Allways pass in memory */
		ainfo->offset = *stack_size;
		*stack_size += ALIGN_TO (size, 8);
		ainfo->storage = is_return ? ArgValuetypeAddrInIReg : ArgOnStack;
		if (!is_return)
			ainfo->arg_size = ALIGN_TO (size, 8);

		g_array_free (fields_array, TRUE);
		return;
	}

	if (size > 8)
		nquads = 2;
	else
		nquads = 1;

	if (!sig->pinvoke) {
		int n = mono_class_value_size (klass, NULL);

		quadsize [0] = n >= 8 ? 8 : n;
		quadsize [1] = n >= 8 ? MAX (n - 8, 8) : 0;

		/* Always pass in 1 or 2 integer registers */
		args [0] = ARG_CLASS_INTEGER;
		args [1] = ARG_CLASS_INTEGER;
		/* Only the simplest cases are supported */
		if (is_return && nquads != 1) {
			args [0] = ARG_CLASS_MEMORY;
			args [1] = ARG_CLASS_MEMORY;
		}
	} else {
		/*
		 * Implement the algorithm from section 3.2.3 of the X86_64 ABI.
		 * The X87 and SSEUP stuff is left out since there are no such types in
		 * the CLR.
		 */
		if (!nfields) {
			ainfo->storage = ArgValuetypeInReg;
			ainfo->pair_storage [0] = ainfo->pair_storage [1] = ArgNone;
			return;
		}

		if (struct_size > 16) {
			ainfo->offset = *stack_size;
			*stack_size += ALIGN_TO (struct_size, 8);
			ainfo->storage = is_return ? ArgValuetypeAddrInIReg : ArgOnStack;
			if (!is_return)
				ainfo->arg_size = ALIGN_TO (struct_size, 8);

			g_array_free (fields_array, TRUE);
			return;
		}

		args [0] = ARG_CLASS_NO_CLASS;
		args [1] = ARG_CLASS_NO_CLASS;
		for (quad = 0; quad < nquads; ++quad) {
			ArgumentClass class1;

			if (nfields == 0)
				class1 = ARG_CLASS_MEMORY;
			else
				class1 = ARG_CLASS_NO_CLASS;
			for (i = 0; i < nfields; ++i) {
				if ((fields [i].offset < 8) && (fields [i].offset + fields [i].size) > 8) {
					/* Unaligned field */
					NOT_IMPLEMENTED;
				}

				/* Skip fields in other quad */
				if ((quad == 0) && (fields [i].offset >= 8))
					continue;
				if ((quad == 1) && (fields [i].offset < 8))
					continue;

				/* How far into this quad this data extends.*/
				/* (8 is size of quad) */
				quadsize [quad] = fields [i].offset + fields [i].size - (quad * 8);

				class1 = merge_argument_class_from_type (fields [i].type, class1);
			}
			/* Empty structs have a nonzero size, causing this assert to be hit */
			if (sig->pinvoke)
				g_assert (class1 != ARG_CLASS_NO_CLASS);
			args [quad] = class1;
		}
	}

	g_array_free (fields_array, TRUE);

	/* Post merger cleanup */
	if ((args [0] == ARG_CLASS_MEMORY) || (args [1] == ARG_CLASS_MEMORY))
		args [0] = args [1] = ARG_CLASS_MEMORY;

	/* Allocate registers */
	{
		int orig_gr = *gr;
		int orig_fr = *fr;

		while (quadsize [0] != 1 && quadsize [0] != 2 && quadsize [0] != 4 && quadsize [0] != 8)
			quadsize [0] ++;
		while (quadsize [1] != 0 && quadsize [1] != 1 && quadsize [1] != 2 && quadsize [1] != 4 && quadsize [1] != 8)
			quadsize [1] ++;

		ainfo->storage = ArgValuetypeInReg;
		ainfo->pair_storage [0] = ainfo->pair_storage [1] = ArgNone;
		g_assert (quadsize [0] <= 8);
		g_assert (quadsize [1] <= 8);
		ainfo->pair_size [0] = quadsize [0];
		ainfo->pair_size [1] = quadsize [1];
		ainfo->nregs = nquads;
		for (quad = 0; quad < nquads; ++quad) {
			switch (args [quad]) {
			case ARG_CLASS_INTEGER:
				if (*gr >= PARAM_REGS)
					args [quad] = ARG_CLASS_MEMORY;
				else {
					ainfo->pair_storage [quad] = ArgInIReg;
					if (is_return)
						ainfo->pair_regs [quad] = return_regs [*gr];
					else
						ainfo->pair_regs [quad] = param_regs [*gr];
					(*gr) ++;
				}
				break;
			case ARG_CLASS_SSE:
				if (*fr >= FLOAT_PARAM_REGS)
					args [quad] = ARG_CLASS_MEMORY;
				else {
					if (quadsize[quad] <= 4)
						ainfo->pair_storage [quad] = ArgInFloatSSEReg;
					else ainfo->pair_storage [quad] = ArgInDoubleSSEReg;
					ainfo->pair_regs [quad] = *fr;
					(*fr) ++;
				}
				break;
			case ARG_CLASS_MEMORY:
				break;
			case ARG_CLASS_NO_CLASS:
				break;
			default:
				g_assert_not_reached ();
			}
		}

		if ((args [0] == ARG_CLASS_MEMORY) || (args [1] == ARG_CLASS_MEMORY)) {
			int arg_size;
			/* Revert possible register assignments */
			*gr = orig_gr;
			*fr = orig_fr;

			ainfo->offset = *stack_size;
			if (sig->pinvoke)
				arg_size = ALIGN_TO (struct_size, 8);
			else
				arg_size = nquads * sizeof (target_mgreg_t);
			*stack_size += arg_size;
			ainfo->storage = is_return ? ArgValuetypeAddrInIReg : ArgOnStack;
			if (!is_return)
				ainfo->arg_size = arg_size;
		}
	}
#endif /* !TARGET_WIN32 */
}

/*
 * mono_arch_get_call_info:
 *
 * Obtain information about a call according to the calling convention.
 * For AMD64 System V, see the "System V ABI, x86-64 Architecture Processor Supplement
 * Draft Version 0.23" document for more information.
 * For AMD64 Windows, see "Overview of x64 Calling Conventions",
 * https://msdn.microsoft.com/en-us/library/ms235286.aspx
 */
CallInfo*
mono_arch_get_call_info (MonoMemPool *mp, MonoMethodSignature *sig)
{
	guint32 i, gr, fr, pstart;
	MonoType *ret_type;
	int n = sig->hasthis + sig->param_count;
	guint32 stack_size = 0;
	CallInfo *cinfo;
	gboolean is_pinvoke = sig->pinvoke;

	if (mp)
		cinfo = (CallInfo *)mono_mempool_alloc0 (mp, sizeof (CallInfo) + (sizeof (ArgInfo) * n));
	else
		cinfo = (CallInfo *)g_malloc0 (sizeof (CallInfo) + (sizeof (ArgInfo) * n));

	cinfo->nargs = n;
	cinfo->gsharedvt = mini_is_gsharedvt_variable_signature (sig);

	gr = 0;
	fr = 0;

#ifdef TARGET_WIN32
	/* Reserve space where the callee can save the argument registers */
	stack_size = 4 * sizeof (target_mgreg_t);
#endif

	/* return value */
	ret_type = mini_get_underlying_type (sig->ret);
	switch (ret_type->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
	case MONO_TYPE_OBJECT:
		cinfo->ret.storage = ArgInIReg;
		cinfo->ret.reg = AMD64_RAX;
		break;
	case MONO_TYPE_U8:
	case MONO_TYPE_I8:
		cinfo->ret.storage = ArgInIReg;
		cinfo->ret.reg = AMD64_RAX;
		break;
	case MONO_TYPE_R4:
		cinfo->ret.storage = ArgInFloatSSEReg;
		cinfo->ret.reg = AMD64_XMM0;
		break;
	case MONO_TYPE_R8:
		cinfo->ret.storage = ArgInDoubleSSEReg;
		cinfo->ret.reg = AMD64_XMM0;
		break;
	case MONO_TYPE_GENERICINST:
		if (!mono_type_generic_inst_is_valuetype (ret_type)) {
			cinfo->ret.storage = ArgInIReg;
			cinfo->ret.reg = AMD64_RAX;
			break;
		}
		if (mini_is_gsharedvt_type (ret_type)) {
			cinfo->ret.storage = ArgGsharedvtVariableInReg;
			break;
		}
		/* fall through */
	case MONO_TYPE_VALUETYPE:
	case MONO_TYPE_TYPEDBYREF: {
		guint32 tmp_gr = 0, tmp_fr = 0, tmp_stacksize = 0;

		add_valuetype (sig, &cinfo->ret, ret_type, TRUE, &tmp_gr, &tmp_fr, &tmp_stacksize);
		g_assert (cinfo->ret.storage != ArgInIReg);
		break;
	}
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		g_assert (mini_is_gsharedvt_type (ret_type));
		cinfo->ret.storage = ArgGsharedvtVariableInReg;
		break;
	case MONO_TYPE_VOID:
		break;
	default:
		g_error ("Can't handle as return value 0x%x", ret_type->type);
	}

	pstart = 0;
	/*
	 * To simplify get_this_arg_reg () and LLVM integration, emit the vret arg after
	 * the first argument, allowing 'this' to be always passed in the first arg reg.
	 * Also do this if the first argument is a reference type, since virtual calls
	 * are sometimes made using calli without sig->hasthis set, like in the delegate
	 * invoke wrappers.
	 */
	ArgStorage ret_storage = cinfo->ret.storage;
	if ((ret_storage == ArgValuetypeAddrInIReg || ret_storage == ArgGsharedvtVariableInReg) && !is_pinvoke && (sig->hasthis || (sig->param_count > 0 && MONO_TYPE_IS_REFERENCE (mini_get_underlying_type (sig->params [0]))))) {
		if (sig->hasthis) {
			add_general (&gr, &stack_size, cinfo->args + 0);
		} else {
			add_general (&gr, &stack_size, &cinfo->args [sig->hasthis + 0]);
			pstart = 1;
		}
		add_general (&gr, &stack_size, &cinfo->ret);
		cinfo->ret.storage = ret_storage;
		cinfo->vret_arg_index = 1;
	} else {
		/* this */
		if (sig->hasthis)
			add_general (&gr, &stack_size, cinfo->args + 0);

		if (ret_storage == ArgValuetypeAddrInIReg || ret_storage == ArgGsharedvtVariableInReg) {
			add_general (&gr, &stack_size, &cinfo->ret);
			cinfo->ret.storage = ret_storage;
		}
	}

	if (!sig->pinvoke && (sig->call_convention == MONO_CALL_VARARG) && (n == 0)) {
		gr = PARAM_REGS;
		fr = FLOAT_PARAM_REGS;
		
		/* Emit the signature cookie just before the implicit arguments */
		add_general (&gr, &stack_size, &cinfo->sig_cookie);
	}

	for (i = pstart; i < sig->param_count; ++i) {
		ArgInfo *ainfo = &cinfo->args [sig->hasthis + i];
		MonoType *ptype;

#ifdef TARGET_WIN32
		/* The float param registers and other param registers must be the same index on Windows x64.*/
		if (gr > fr)
			fr = gr;
		else if (fr > gr)
			gr = fr;
#endif

		if (!sig->pinvoke && (sig->call_convention == MONO_CALL_VARARG) && (i == sig->sentinelpos)) {
			/* We allways pass the sig cookie on the stack for simplicity */
			/* 
			 * Prevent implicit arguments + the sig cookie from being passed 
			 * in registers.
			 */
			gr = PARAM_REGS;
			fr = FLOAT_PARAM_REGS;

			/* Emit the signature cookie just before the implicit arguments */
			add_general (&gr, &stack_size, &cinfo->sig_cookie);
		}

		ptype = mini_get_underlying_type (sig->params [i]);
		switch (ptype->type) {
		case MONO_TYPE_I1:
			ainfo->is_signed = 1;
		case MONO_TYPE_U1:
			add_general (&gr, &stack_size, ainfo);
			ainfo->byte_arg_size = 1;
			break;
		case MONO_TYPE_I2:
			ainfo->is_signed = 1;
		case MONO_TYPE_U2:
			add_general (&gr, &stack_size, ainfo);
			ainfo->byte_arg_size = 2;
			break;
		case MONO_TYPE_I4:
			ainfo->is_signed = 1;
		case MONO_TYPE_U4:
			add_general (&gr, &stack_size, ainfo);
			ainfo->byte_arg_size = 4;
			break;
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_PTR:
		case MONO_TYPE_FNPTR:
		case MONO_TYPE_OBJECT:
			add_general (&gr, &stack_size, ainfo);
			break;
		case MONO_TYPE_GENERICINST:
			if (!mono_type_generic_inst_is_valuetype (ptype)) {
				add_general (&gr, &stack_size, ainfo);
				break;
			}
			if (mini_is_gsharedvt_variable_type (ptype)) {
				/* gsharedvt arguments are passed by ref */
				add_general (&gr, &stack_size, ainfo);
				if (ainfo->storage == ArgInIReg)
					ainfo->storage = ArgGSharedVtInReg;
				else
					ainfo->storage = ArgGSharedVtOnStack;
				break;
			}
			/* fall through */
		case MONO_TYPE_VALUETYPE:
		case MONO_TYPE_TYPEDBYREF:
			add_valuetype (sig, ainfo, ptype, FALSE, &gr, &fr, &stack_size);
			break;
		case MONO_TYPE_U8:

		case MONO_TYPE_I8:
			add_general (&gr, &stack_size, ainfo);
			break;
		case MONO_TYPE_R4:
			add_float (&fr, &stack_size, ainfo, FALSE);
			break;
		case MONO_TYPE_R8:
			add_float (&fr, &stack_size, ainfo, TRUE);
			break;
		case MONO_TYPE_VAR:
		case MONO_TYPE_MVAR:
			/* gsharedvt arguments are passed by ref */
			g_assert (mini_is_gsharedvt_type (ptype));
			add_general (&gr, &stack_size, ainfo);
			if (ainfo->storage == ArgInIReg)
				ainfo->storage = ArgGSharedVtInReg;
			else
				ainfo->storage = ArgGSharedVtOnStack;
			break;
		default:
			g_assert_not_reached ();
		}
	}

	if (!sig->pinvoke && (sig->call_convention == MONO_CALL_VARARG) && (n > 0) && (sig->sentinelpos == sig->param_count)) {
		gr = PARAM_REGS;
		fr = FLOAT_PARAM_REGS;
		
		/* Emit the signature cookie just before the implicit arguments */
		add_general (&gr, &stack_size, &cinfo->sig_cookie);
	}

	cinfo->stack_usage = stack_size;
	cinfo->reg_usage = gr;
	cinfo->freg_usage = fr;
	return cinfo;
}

/*
 * Initialize the cpu to execute managed code.
 */
void
mono_arch_cpu_init (void)
{
#ifndef _MSC_VER
	guint16 fpcw;

	/* spec compliance requires running with double precision */
	__asm__  __volatile__ ("fnstcw %0\n": "=m" (fpcw));
	fpcw &= ~X86_FPCW_PRECC_MASK;
	fpcw |= X86_FPCW_PREC_DOUBLE;
	__asm__  __volatile__ ("fldcw %0\n": : "m" (fpcw));
	__asm__  __volatile__ ("fnstcw %0\n": "=m" (fpcw));
#else
	/* TODO: This is crashing on Win64 right now.
	* _control87 (_PC_53, MCW_PC);
	*/
#endif
}

/*
 * Initialize architecture specific code.
 */
void
mono_arch_init (void)
{
	if (!mono_aot_only)
		mono_amd64_bp_trampoline = mini_get_breakpoint_trampoline ();
}

/*
 * Cleanup architecture specific code.
 */
void
mono_arch_cleanup (void)
{
}

/*
 * This function returns the optimizations supported on this cpu.
 */
guint32
mono_arch_cpu_optimizations (guint32 *exclude_mask)
{
	guint32 opts = 0;

	*exclude_mask = 0;

	if (mono_hwcap_x86_has_cmov) {
		opts |= MONO_OPT_CMOV;

		if (mono_hwcap_x86_has_fcmov)
			opts |= MONO_OPT_FCMOV;
		else
			*exclude_mask |= MONO_OPT_FCMOV;
	} else {
		*exclude_mask |= MONO_OPT_CMOV;
	}

	return opts;
}

MonoCPUFeatures
mono_arch_get_cpu_features (void)
{
	guint64 features = MONO_CPU_INITED;

	if (mono_hwcap_x86_has_sse1)
		features |= MONO_CPU_X86_SSE;

	if (mono_hwcap_x86_has_sse2)
		features |= MONO_CPU_X86_SSE2;

	if (mono_hwcap_x86_has_sse3)
		features |= MONO_CPU_X86_SSE3;

	if (mono_hwcap_x86_has_ssse3)
		features |= MONO_CPU_X86_SSSE3;

	if (mono_hwcap_x86_has_sse41)
		features |= MONO_CPU_X86_SSE41;

	if (mono_hwcap_x86_has_sse42)
		features |= MONO_CPU_X86_SSE42;

	if (mono_hwcap_x86_has_popcnt)
		features |= MONO_CPU_X86_POPCNT;

	if (mono_hwcap_x86_has_lzcnt)
		features |= MONO_CPU_X86_LZCNT;

	return (MonoCPUFeatures)features;
}

typedef struct {
	MonoMethodSignature *sig;
	CallInfo *cinfo;
	int nstack_args, nullable_area;
} ArchDynCallInfo;

static gboolean
dyn_call_supported (MonoMethodSignature *sig, CallInfo *cinfo)
{
	int i;

	switch (cinfo->ret.storage) {
	case ArgNone:
	case ArgInIReg:
	case ArgInFloatSSEReg:
	case ArgInDoubleSSEReg:
	case ArgValuetypeAddrInIReg:
	case ArgValuetypeInReg:
		break;
	default:
		return FALSE;
	}

	for (i = 0; i < cinfo->nargs; ++i) {
		ArgInfo *ainfo = &cinfo->args [i];
		switch (ainfo->storage) {
		case ArgInIReg:
		case ArgInFloatSSEReg:
		case ArgInDoubleSSEReg:
		case ArgValuetypeInReg:
		case ArgValuetypeAddrInIReg:
		case ArgValuetypeAddrOnStack:
		case ArgOnStack:
			break;
		default:
			return FALSE;
		}
	}

	return TRUE;
}

/*
 * mono_arch_dyn_call_prepare:
 *
 *   Return a pointer to an arch-specific structure which contains information 
 * needed by mono_arch_get_dyn_call_args (). Return NULL if OP_DYN_CALL is not
 * supported for SIG.
 * This function is equivalent to ffi_prep_cif in libffi.
 */
MonoDynCallInfo*
mono_arch_dyn_call_prepare (MonoMethodSignature *sig)
{
	ArchDynCallInfo *info;
	CallInfo *cinfo;
	int i, aindex;

	cinfo = mono_arch_get_call_info (NULL, sig);

	if (!dyn_call_supported (sig, cinfo)) {
		g_free (cinfo);
		return NULL;
	}

	info = g_new0 (ArchDynCallInfo, 1);
	// FIXME: Preprocess the info to speed up get_dyn_call_args ().
	info->sig = sig;
	info->cinfo = cinfo;
	info->nstack_args = 0;

	for (i = 0; i < cinfo->nargs; ++i) {
		ArgInfo *ainfo = &cinfo->args [i];
		switch (ainfo->storage) {
		case ArgOnStack:
		case ArgValuetypeAddrOnStack:
			info->nstack_args = MAX (info->nstack_args, (ainfo->offset / sizeof (target_mgreg_t)) + (ainfo->arg_size / sizeof (target_mgreg_t)));
			break;
		default:
			break;
		}
	}

	for (aindex = 0; aindex < sig->param_count; aindex++) {
		MonoType *t = sig->params [aindex];
		ArgInfo *ainfo = &cinfo->args [aindex + sig->hasthis];

		if (t->byref)
			continue;

		switch (t->type) {
		case MONO_TYPE_GENERICINST:
			if (t->type == MONO_TYPE_GENERICINST && mono_class_is_nullable (mono_class_from_mono_type_internal (t))) {
				MonoClass *klass = mono_class_from_mono_type_internal (t);
				int size;

				if (!(ainfo->storage == ArgValuetypeInReg || ainfo->storage == ArgOnStack)) {
					/* Nullables need a temporary buffer, its stored at the end of DynCallArgs.regs after the stack args */
					size = mono_class_value_size (klass, NULL);
					info->nullable_area += size;
				}
			}
			break;
		default:
			break;
		}
	}

	info->nullable_area = ALIGN_TO (info->nullable_area, 16);

	/* Align to 16 bytes */
	if (info->nstack_args & 1)
		info->nstack_args ++;
	
	return (MonoDynCallInfo*)info;
}

/*
 * mono_arch_dyn_call_free:
 *
 *   Free a MonoDynCallInfo structure.
 */
void
mono_arch_dyn_call_free (MonoDynCallInfo *info)
{
	ArchDynCallInfo *ainfo = (ArchDynCallInfo*)info;

	g_free (ainfo->cinfo);
	g_free (ainfo);
}

int
mono_arch_dyn_call_get_buf_size (MonoDynCallInfo *info)
{
	ArchDynCallInfo *ainfo = (ArchDynCallInfo*)info;

	/* Extend the 'regs' field dynamically */
	return sizeof (DynCallArgs) + (ainfo->nstack_args * sizeof (target_mgreg_t)) + ainfo->nullable_area;
}

#define PTR_TO_GREG(ptr) ((host_mgreg_t)(ptr))
#define GREG_TO_PTR(greg) ((gpointer)(greg))

/*
 * mono_arch_get_start_dyn_call:
 *
 *   Convert the arguments ARGS to a format which can be passed to OP_DYN_CALL, and
 * store the result into BUF.
 * ARGS should be an array of pointers pointing to the arguments.
 * RET should point to a memory buffer large enought to hold the result of the
 * call.
 * This function should be as fast as possible, any work which does not depend
 * on the actual values of the arguments should be done in 
 * mono_arch_dyn_call_prepare ().
 * start_dyn_call + OP_DYN_CALL + finish_dyn_call is equivalent to ffi_call in
 * libffi.
 */
void
mono_arch_start_dyn_call (MonoDynCallInfo *info, gpointer **args, guint8 *ret, guint8 *buf)
{
	ArchDynCallInfo *dinfo = (ArchDynCallInfo*)info;
	DynCallArgs *p = (DynCallArgs*)buf;
	int arg_index, greg, i, pindex;
	MonoMethodSignature *sig = dinfo->sig;
	int buffer_offset = 0;
	guint8 *nullable_buffer;
	static int general_param_reg_to_index [MONO_MAX_IREGS];
	static int float_param_reg_to_index [MONO_MAX_FREGS];

	static gboolean param_reg_to_index_inited;

	if (!param_reg_to_index_inited) {
		for (i = 0; i < PARAM_REGS; ++i)
			general_param_reg_to_index [param_regs[i]] = i;
		for (i = 0; i < FLOAT_PARAM_REGS; ++i)
			float_param_reg_to_index [float_param_regs[i]] = i;
		mono_memory_barrier ();
		param_reg_to_index_inited = 1;
	} else {
		mono_memory_barrier ();
	}

	p->res = 0;
	p->ret = ret;
	p->nstack_args = dinfo->nstack_args;

	arg_index = 0;
	greg = 0;
	pindex = 0;

	/* Stored after the stack arguments */
	nullable_buffer = (guint8*)&(p->regs [PARAM_REGS + dinfo->nstack_args]);

	if (sig->hasthis || dinfo->cinfo->vret_arg_index == 1) {
		p->regs [greg ++] = PTR_TO_GREG(*(args [arg_index ++]));
		if (!sig->hasthis)
			pindex = 1;
	}

	if (dinfo->cinfo->ret.storage == ArgValuetypeAddrInIReg || dinfo->cinfo->ret.storage == ArgGsharedvtVariableInReg)
		p->regs [greg ++] = PTR_TO_GREG (ret);

	for (; pindex < sig->param_count; pindex++) {
		MonoType *t = mini_get_underlying_type (sig->params [pindex]);
		gpointer *arg = args [arg_index ++];
		ArgInfo *ainfo = &dinfo->cinfo->args [pindex + sig->hasthis];
		int slot;

		if (ainfo->storage == ArgOnStack || ainfo->storage == ArgValuetypeAddrOnStack) {
			slot = PARAM_REGS + (ainfo->offset / sizeof (target_mgreg_t));
		} else if (ainfo->storage == ArgValuetypeAddrInIReg) {
			g_assert (ainfo->pair_storage [0] == ArgInIReg && ainfo->pair_storage [1] == ArgNone);
			slot = general_param_reg_to_index [ainfo->pair_regs [0]];
		} else if (ainfo->storage == ArgInFloatSSEReg || ainfo->storage == ArgInDoubleSSEReg) {
			slot = float_param_reg_to_index [ainfo->reg];
		} else {
			slot = general_param_reg_to_index [ainfo->reg];
		}

		if (t->byref) {
			p->regs [slot] = PTR_TO_GREG (*(arg));
			continue;
		}

		switch (t->type) {
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_PTR:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
#if !defined(MONO_ARCH_ILP32)
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
#endif
			p->regs [slot] = PTR_TO_GREG (*(arg));
			break;
#if defined(MONO_ARCH_ILP32)
		case MONO_TYPE_I8:
		case MONO_TYPE_U8:
			p->regs [slot] = *(guint64*)(arg);
			break;
#endif
		case MONO_TYPE_U1:
			p->regs [slot] = *(guint8*)(arg);
			break;
		case MONO_TYPE_I1:
			p->regs [slot] = *(gint8*)(arg);
			break;
		case MONO_TYPE_I2:
			p->regs [slot] = *(gint16*)(arg);
			break;
		case MONO_TYPE_U2:
			p->regs [slot] = *(guint16*)(arg);
			break;
		case MONO_TYPE_I4:
			p->regs [slot] = *(gint32*)(arg);
			break;
		case MONO_TYPE_U4:
			p->regs [slot] = *(guint32*)(arg);
			break;
		case MONO_TYPE_R4: {
			double d;
			*(float*)&d = *(float*)(arg);

			if (ainfo->storage == ArgOnStack) {
				*(double *)(p->regs + slot) = d;
			} else {
				p->has_fp = 1;
				p->fregs [slot] = d;
			}
			break;
		}
		case MONO_TYPE_R8:
			if (ainfo->storage == ArgOnStack) {
				*(double *)(p->regs + slot) = *(double*)(arg);
			} else {
				p->has_fp = 1;
				p->fregs [slot] = *(double*)(arg);
			}
			break;
		case MONO_TYPE_GENERICINST:
		    if (MONO_TYPE_IS_REFERENCE (t)) {
				p->regs [slot] = PTR_TO_GREG (*(arg));
				break;
			} else if (t->type == MONO_TYPE_GENERICINST && mono_class_is_nullable (mono_class_from_mono_type_internal (t))) {
					MonoClass *klass = mono_class_from_mono_type_internal (t);
					guint8 *nullable_buf;
					int size;

					size = mono_class_value_size (klass, NULL);
					if (ainfo->storage == ArgValuetypeInReg || ainfo->storage == ArgOnStack) {
						nullable_buf = g_alloca (size);
					} else {
						nullable_buf = nullable_buffer + buffer_offset;
						buffer_offset += size;
						g_assert (buffer_offset <= dinfo->nullable_area);
					}

					/* The argument pointed to by arg is either a boxed vtype or null */
					mono_nullable_init (nullable_buf, (MonoObject*)arg, klass);

					arg = (gpointer*)nullable_buf;
					/* Fall though */

			} else {
				/* Fall through */
			}
		case MONO_TYPE_VALUETYPE: {
			switch (ainfo->storage) {
			case ArgValuetypeInReg:
				for (i = 0; i < 2; ++i) {
					switch (ainfo->pair_storage [i]) {
					case ArgNone:
						break;
					case ArgInIReg:
						slot = general_param_reg_to_index [ainfo->pair_regs [i]];
						p->regs [slot] = ((target_mgreg_t*)(arg))[i];
						break;
					case ArgInFloatSSEReg: {
						double d;
						p->has_fp = 1;
						slot = float_param_reg_to_index [ainfo->pair_regs [i]];
						*(float*)&d = ((float*)(arg))[i];
						p->fregs [slot] = d;
						break;
					}
					case ArgInDoubleSSEReg:
						p->has_fp = 1;
						slot = float_param_reg_to_index [ainfo->pair_regs [i]];
						p->fregs [slot] = ((double*)(arg))[i];
						break;
					default:
						g_assert_not_reached ();
						break;
					}
				}
				break;
			case ArgValuetypeAddrInIReg:
			case ArgValuetypeAddrOnStack:
				// In DYNCALL use case value types are already copied when included in parameter array.
				// Currently no need to make an extra temporary value type on stack for this use case.
				p->regs [slot] = (target_mgreg_t)arg;
				break;
			case ArgOnStack:
				for (i = 0; i < ainfo->arg_size / 8; ++i)
					p->regs [slot + i] = ((target_mgreg_t*)(arg))[i];
				break;
			default:
				g_assert_not_reached ();
				break;
			}
			break;
		}
		default:
			g_assert_not_reached ();
		}
	}
}

/*
 * mono_arch_finish_dyn_call:
 *
 *   Store the result of a dyn call into the return value buffer passed to
 * start_dyn_call ().
 * This function should be as fast as possible, any work which does not depend
 * on the actual values of the arguments should be done in 
 * mono_arch_dyn_call_prepare ().
 */
void
mono_arch_finish_dyn_call (MonoDynCallInfo *info, guint8 *buf)
{
	ArchDynCallInfo *dinfo = (ArchDynCallInfo*)info;
	MonoMethodSignature *sig = dinfo->sig;
	DynCallArgs *dargs = (DynCallArgs*)buf;
	guint8 *ret = dargs->ret;
	host_mgreg_t res = dargs->res;
	MonoType *sig_ret = mini_get_underlying_type (sig->ret);
	int i;

	switch (sig_ret->type) {
	case MONO_TYPE_VOID:
		*(gpointer*)ret = NULL;
		break;
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
		*(gpointer*)ret = GREG_TO_PTR (res);
		break;
	case MONO_TYPE_I1:
		*(gint8*)ret = res;
		break;
	case MONO_TYPE_U1:
		*(guint8*)ret = res;
		break;
	case MONO_TYPE_I2:
		*(gint16*)ret = res;
		break;
	case MONO_TYPE_U2:
		*(guint16*)ret = res;
		break;
	case MONO_TYPE_I4:
		*(gint32*)ret = res;
		break;
	case MONO_TYPE_U4:
		*(guint32*)ret = res;
		break;
	case MONO_TYPE_I8:
		*(gint64*)ret = res;
		break;
	case MONO_TYPE_U8:
		*(guint64*)ret = res;
		break;
	case MONO_TYPE_R4:
		*(float*)ret = *(float*)&(dargs->fregs [0]);
		break;
	case MONO_TYPE_R8:
		*(double*)ret = dargs->fregs [0];
		break;
	case MONO_TYPE_GENERICINST:
		if (MONO_TYPE_IS_REFERENCE (sig_ret)) {
			*(gpointer*)ret = GREG_TO_PTR(res);
			break;
		} else {
			/* Fall through */
		}
	case MONO_TYPE_VALUETYPE:
		if (dinfo->cinfo->ret.storage == ArgValuetypeAddrInIReg || dinfo->cinfo->ret.storage == ArgGsharedvtVariableInReg) {
			/* Nothing to do */
		} else {
			ArgInfo *ainfo = &dinfo->cinfo->ret;

			g_assert (ainfo->storage == ArgValuetypeInReg);

			for (i = 0; i < 2; ++i) {
				switch (ainfo->pair_storage [0]) {
				case ArgInIReg:
					((host_mgreg_t*)ret)[i] = res;
					break;
				case ArgInDoubleSSEReg:
					((double*)ret)[i] = dargs->fregs [i];
					break;
				case ArgNone:
					break;
				default:
					g_assert_not_reached ();
					break;
				}
			}
		}
		break;
	default:
		g_assert_not_reached ();
	}
}

#undef PTR_TO_GREG
#undef GREG_TO_PTR

#ifdef TARGET_MACH
static int tls_gs_offset;
#endif

gboolean
mono_arch_have_fast_tls (void)
{
#ifdef TARGET_MACH
	static gboolean have_fast_tls = FALSE;
	static gboolean inited = FALSE;
	guint8 *ins;

	if (mini_debug_options.use_fallback_tls)
		return FALSE;

	if (inited)
		return have_fast_tls;

	ins = (guint8*)pthread_getspecific;

	/*
	 * We're looking for these two instructions:
	 *
	 * mov    %gs:[offset](,%rdi,8),%rax
	 * retq
	 */
	have_fast_tls = ins [0] == 0x65 &&
		       ins [1] == 0x48 &&
		       ins [2] == 0x8b &&
		       ins [3] == 0x04 &&
		       ins [4] == 0xfd &&
		       ins [6] == 0x00 &&
		       ins [7] == 0x00 &&
		       ins [8] == 0x00 &&
		       ins [9] == 0xc3;

	tls_gs_offset = ins[5];

	/*
	 * Apple now loads a different version of pthread_getspecific when launched from Xcode
	 * For that version we're looking for these instructions:
	 *
	 * pushq  %rbp
	 * movq   %rsp, %rbp
	 * mov    %gs:[offset](,%rdi,8),%rax
	 * popq   %rbp
	 * retq
	 */
	if (!have_fast_tls) {
		have_fast_tls = ins [0] == 0x55 &&
			       ins [1] == 0x48 &&
			       ins [2] == 0x89 &&
			       ins [3] == 0xe5 &&
			       ins [4] == 0x65 &&
			       ins [5] == 0x48 &&
			       ins [6] == 0x8b &&
			       ins [7] == 0x04 &&
			       ins [8] == 0xfd &&
			       ins [10] == 0x00 &&
			       ins [11] == 0x00 &&
			       ins [12] == 0x00 &&
			       ins [13] == 0x5d &&
			       ins [14] == 0xc3;

		tls_gs_offset = ins[9];
	}
	inited = TRUE;

	return have_fast_tls;
#elif defined(TARGET_ANDROID)
	return FALSE;
#else
	if (mini_debug_options.use_fallback_tls)
		return FALSE;
	return TRUE;
#endif
}

int
mono_amd64_get_tls_gs_offset (void)
{
#ifdef TARGET_OSX
	return tls_gs_offset;
#else
	g_assert_not_reached ();
	return -1;
#endif
}

/*
 * \param code buffer to store code to
 * \param dreg hard register where to place the result
 * \param tls_offset offset info
 * \return a pointer to the end of the stored code
 *
 * mono_amd64_emit_tls_get emits in \p code the native code that puts in
 * the dreg register the item in the thread local storage identified
 * by tls_offset.
 */
guint8*
mono_amd64_emit_tls_get (guint8* code, int dreg, int tls_offset)
{
#ifdef TARGET_WIN32
	if (tls_offset < 64) {
		x86_prefix (code, X86_GS_PREFIX);
		amd64_mov_reg_mem (code, dreg, (tls_offset * 8) + 0x1480, 8);
	} else {
		guint8 *buf [16];

		g_assert (tls_offset < 0x440);
		/* Load TEB->TlsExpansionSlots */
		x86_prefix (code, X86_GS_PREFIX);
		amd64_mov_reg_mem (code, dreg, 0x1780, 8);
		amd64_test_reg_reg (code, dreg, dreg);
		buf [0] = code;
		amd64_branch (code, X86_CC_EQ, code, TRUE);
		amd64_mov_reg_membase (code, dreg, dreg, (tls_offset * 8) - 0x200, 8);
		amd64_patch (buf [0], code);
	}
#elif defined(TARGET_MACH)
	x86_prefix (code, X86_GS_PREFIX);
	amd64_mov_reg_mem (code, dreg, tls_gs_offset + (tls_offset * 8), 8);
#else
	if (mono_amd64_optimize_for_xen) {
		x86_prefix (code, X86_FS_PREFIX);
		amd64_mov_reg_mem (code, dreg, 0, 8);
		amd64_mov_reg_membase (code, dreg, dreg, tls_offset, 8);
	} else {
		x86_prefix (code, X86_FS_PREFIX);
		amd64_mov_reg_mem (code, dreg, tls_offset, 8);
	}
#endif
	return code;
}

guint8*
mono_amd64_emit_tls_set (guint8 *code, int sreg, int tls_offset)
{
#ifdef TARGET_WIN32
	g_assert_not_reached ();
#elif defined(TARGET_MACH)
	x86_prefix (code, X86_GS_PREFIX);
	amd64_mov_mem_reg (code, tls_gs_offset + (tls_offset * 8), sreg, 8);
#else
	g_assert (!mono_amd64_optimize_for_xen);
	x86_prefix (code, X86_FS_PREFIX);
	amd64_mov_mem_reg (code, tls_offset, sreg, 8);
#endif
	return code;
}

G_BEGIN_DECLS
void __chkstk (void);
void ___chkstk_ms (void);
G_END_DECLS

void
mono_arch_register_lowlevel_calls (void)
{
	/* The signature doesn't matter */
	mono_register_jit_icall (mono_amd64_throw_exception, mono_icall_sig_void, TRUE);

#if defined(TARGET_WIN32) || defined(HOST_WIN32)
#if _MSC_VER
	mono_register_jit_icall_info (&mono_get_jit_icall_info ()->mono_chkstk_win64, __chkstk, "mono_chkstk_win64", NULL, TRUE, "__chkstk");
#else
	mono_register_jit_icall_info (&mono_get_jit_icall_info ()->mono_chkstk_win64, ___chkstk_ms, "mono_chkstk_win64", NULL, TRUE, "___chkstk_ms");
#endif
#endif
}

MONO_NEVER_INLINE
void
mono_arch_flush_icache (guint8 *code, gint size)
{
	/* call/ret required (or likely other control transfer) */
}

void
mono_arch_flush_register_windows (void)
{
}

/*
 * Determine whenever the trap whose info is in SIGINFO is caused by
 * integer overflow.
 */
gboolean
mono_arch_is_int_overflow (void *sigctx, void *info)
{
	MonoContext ctx;
	guint8* rip;
	int reg;
	gint64 value;

	mono_sigctx_to_monoctx (sigctx, &ctx);

	rip = (guint8*)ctx.gregs [AMD64_RIP];

	if (IS_REX (rip [0])) {
		reg = amd64_rex_b (rip [0]);
		rip ++;
	}
	else
		reg = 0;

	if ((rip [0] == 0xf7) && (x86_modrm_mod (rip [1]) == 0x3) && (x86_modrm_reg (rip [1]) == 0x7)) {
		/* idiv REG */
		reg += x86_modrm_rm (rip [1]);

		value = ctx.gregs [reg];

		if (value == -1)
			return TRUE;
	}

	return FALSE;
}

/**
 * \return TRUE if no sw breakpoint was present.
 *
 * Copy \p size bytes from \p code - \p offset to the buffer \p buf. If the debugger inserted software
 * breakpoints in the original code, they are removed in the copy.
 */
gboolean
mono_breakpoint_clean_code (guint8 *method_start, guint8 *code, int offset, guint8 *buf, int size)
{
	/*
	 * If method_start is non-NULL we need to perform bound checks, since we access memory
	 * at code - offset we could go before the start of the method and end up in a different
	 * page of memory that is not mapped or read incorrect data anyway. We zero-fill the bytes
	 * instead.
	 */
	if (!method_start || code - offset >= method_start) {
		memcpy (buf, code - offset, size);
	} else {
		int diff = code - method_start;
		memset (buf, 0, size);
		memcpy (buf + offset - diff, method_start, diff + size - offset);
	}
	return TRUE;
}

int
mono_arch_get_this_arg_reg (guint8 *code)
{
	return AMD64_ARG_REG1;
}

gpointer
mono_arch_get_this_arg_from_call (host_mgreg_t *regs, guint8 *code)
{
	return (gpointer)regs [mono_arch_get_this_arg_reg (code)];
}

#define MAX_ARCH_DELEGATE_PARAMS 10

static gpointer
get_delegate_invoke_impl (MonoTrampInfo **info, gboolean has_target, guint32 param_count)
{
	guint8 *code, *start;
	GSList *unwind_ops = NULL;
	int i;

	unwind_ops = mono_arch_get_cie_program ();

	const int size = 64;

	start = code = (guint8 *)mono_global_codeman_reserve (size + MONO_TRAMPOLINE_UNWINDINFO_SIZE(0));

	if (has_target) {

		/* Replace the this argument with the target */
		amd64_mov_reg_reg (code, AMD64_RAX, AMD64_ARG_REG1, 8);
		amd64_mov_reg_membase (code, AMD64_ARG_REG1, AMD64_RAX, MONO_STRUCT_OFFSET (MonoDelegate, target), 8);
		amd64_jump_membase (code, AMD64_RAX, MONO_STRUCT_OFFSET (MonoDelegate, method_ptr));

	} else {
		if (param_count == 0) {
			amd64_jump_membase (code, AMD64_ARG_REG1, MONO_STRUCT_OFFSET (MonoDelegate, method_ptr));
		} else {
			/* We have to shift the arguments left */
			amd64_mov_reg_reg (code, AMD64_RAX, AMD64_ARG_REG1, 8);
			for (i = 0; i < param_count; ++i) {
#ifdef TARGET_WIN32
				if (i < 3)
					amd64_mov_reg_reg (code, param_regs [i], param_regs [i + 1], 8);
				else
					amd64_mov_reg_membase (code, param_regs [i], AMD64_RSP, 0x28, 8);
#else
				amd64_mov_reg_reg (code, param_regs [i], param_regs [i + 1], 8);
#endif
			}

			amd64_jump_membase (code, AMD64_RAX, MONO_STRUCT_OFFSET (MonoDelegate, method_ptr));
		}
	}

	g_assertf ((code - start) <= size, "%d %d", (int)(code - start), size);
	g_assert_checked (mono_arch_unwindinfo_validate_size (unwind_ops, MONO_TRAMPOLINE_UNWINDINFO_SIZE(0)));

	mono_arch_flush_icache (start, code - start);

	if (has_target) {
		*info = mono_tramp_info_create ("delegate_invoke_impl_has_target", start, code - start, NULL, unwind_ops);
	} else {
		char *name = g_strdup_printf ("delegate_invoke_impl_target_%d", param_count);
		*info = mono_tramp_info_create (name, start, code - start, NULL, unwind_ops);
		g_free (name);
	}

	if (mono_jit_map_is_enabled ()) {
		char *buff;
		if (has_target)
			buff = (char*)"delegate_invoke_has_target";
		else
			buff = g_strdup_printf ("delegate_invoke_no_target_%d", param_count);
		mono_emit_jit_tramp (start, code - start, buff);
		if (!has_target)
			g_free (buff);
	}
	MONO_PROFILER_RAISE (jit_code_buffer, (start, code - start, MONO_PROFILER_CODE_BUFFER_DELEGATE_INVOKE, NULL));

	return start;
}

gpointer
mono_arch_get_delegate_invoke_impl (MonoMethodSignature *sig, gboolean has_target)
{
	guint8 *code, *start;
	int i;

	if (sig->param_count > MAX_ARCH_DELEGATE_PARAMS)
		return NULL;

	/* FIXME: Support more cases */
	if (MONO_TYPE_ISSTRUCT (mini_get_underlying_type (sig->ret)))
		return NULL;

	if (has_target) {
		static guint8* cached = NULL;

		if (cached)
			return cached;

		if (mono_ee_features.use_aot_trampolines) {
			start = (guint8 *)mono_aot_get_trampoline ("delegate_invoke_impl_has_target");
		} else {
			MonoTrampInfo *info;
			start = (guint8 *)get_delegate_invoke_impl (&info, TRUE, 0);
			mono_tramp_info_register (info, NULL);
		}

		mono_memory_barrier ();

		cached = start;
	} else {
		static guint8* cache [MAX_ARCH_DELEGATE_PARAMS + 1] = {NULL};
		for (i = 0; i < sig->param_count; ++i)
			if (!mono_is_regsize_var (sig->params [i]))
				return NULL;
		if (sig->param_count > 4)
			return NULL;

		code = cache [sig->param_count];
		if (code)
			return code;

		if (mono_ee_features.use_aot_trampolines) {
			char *name = g_strdup_printf ("delegate_invoke_impl_target_%d", sig->param_count);
			start = (guint8 *)mono_aot_get_trampoline (name);
			g_free (name);
		} else {
			MonoTrampInfo *info;
			start = (guint8 *)get_delegate_invoke_impl (&info, FALSE, sig->param_count);
			mono_tramp_info_register (info, NULL);
		}

		mono_memory_barrier ();

		cache [sig->param_count] = start;
	}

	return start;
}

void
mono_arch_finish_init (void)
{
#if !defined(HOST_WIN32) && defined(MONO_XEN_OPT)
	mono_amd64_optimize_for_xen = access ("/proc/xen", F_OK) == 0;
#endif
}

static gboolean
amd64_use_imm32 (gint64 val)
{
	if (mini_debug_options.single_imm_size)
		return FALSE;

	return amd64_is_imm32 (val);
}

#define CMP_SIZE (6 + 1)
#define CMP_REG_REG_SIZE (4 + 1)
#define BR_SMALL_SIZE 2
#define BR_LARGE_SIZE 6
#define MOV_REG_IMM_SIZE 10
#define MOV_REG_IMM_32BIT_SIZE 6
#define JUMP_REG_SIZE (2 + 1)

static int
imt_branch_distance (MonoIMTCheckItem **imt_entries, int start, int target)
{
	int i, distance = 0;
	for (i = start; i < target; ++i)
		distance += imt_entries [i]->chunk_size;
	return distance;
}

/*
 * LOCKING: called with the domain lock held
 */
gpointer
mono_arch_build_imt_trampoline (MonoVTable *vtable, MonoDomain *domain, MonoIMTCheckItem **imt_entries, int count,
	gpointer fail_tramp)
{
	int i;
	int size = 0;
	guint8 *code, *start;
	gboolean vtable_is_32bit = ((gsize)(vtable) == (gsize)(int)(gsize)(vtable));
	GSList *unwind_ops;

	for (i = 0; i < count; ++i) {
		MonoIMTCheckItem *item = imt_entries [i];
		if (item->is_equals) {
			if (item->check_target_idx) {
				if (!item->compare_done) {
					if (amd64_use_imm32 ((gint64)item->key))
						item->chunk_size += CMP_SIZE;
					else
						item->chunk_size += MOV_REG_IMM_SIZE + CMP_REG_REG_SIZE;
				}
				if (item->has_target_code) {
					item->chunk_size += MOV_REG_IMM_SIZE;
				} else {
					if (vtable_is_32bit)
						item->chunk_size += MOV_REG_IMM_32BIT_SIZE;
					else
						item->chunk_size += MOV_REG_IMM_SIZE;
				}
				item->chunk_size += BR_SMALL_SIZE + JUMP_REG_SIZE;
			} else {
				if (fail_tramp) {
					item->chunk_size += MOV_REG_IMM_SIZE * 3 + CMP_REG_REG_SIZE +
						BR_SMALL_SIZE + JUMP_REG_SIZE * 2;
				} else {
					if (vtable_is_32bit)
						item->chunk_size += MOV_REG_IMM_32BIT_SIZE;
					else
						item->chunk_size += MOV_REG_IMM_SIZE;
					item->chunk_size += JUMP_REG_SIZE;
					/* with assert below:
					 * item->chunk_size += CMP_SIZE + BR_SMALL_SIZE + 1;
					 */
				}
			}
		} else {
			if (amd64_use_imm32 ((gint64)item->key))
				item->chunk_size += CMP_SIZE;
			else
				item->chunk_size += MOV_REG_IMM_SIZE + CMP_REG_REG_SIZE;
			item->chunk_size += BR_LARGE_SIZE;
			imt_entries [item->check_target_idx]->compare_done = TRUE;
		}
		size += item->chunk_size;
	}
	if (fail_tramp) {
		code = (guint8 *)mono_method_alloc_generic_virtual_trampoline (mono_domain_ambient_memory_manager (domain), size + MONO_TRAMPOLINE_UNWINDINFO_SIZE(0));
	} else {
		MonoMemoryManager *mem_manager = m_class_get_mem_manager (domain, vtable->klass);
		code = (guint8 *)mono_mem_manager_code_reserve (mem_manager, size + MONO_TRAMPOLINE_UNWINDINFO_SIZE(0));
	}
	start = code;

	unwind_ops = mono_arch_get_cie_program ();

	for (i = 0; i < count; ++i) {
		MonoIMTCheckItem *item = imt_entries [i];
		item->code_target = code;
		if (item->is_equals) {
			gboolean fail_case = !item->check_target_idx && fail_tramp;

			if (item->check_target_idx || fail_case) {
				if (!item->compare_done || fail_case) {
					if (amd64_use_imm32 ((gint64)item->key))
						amd64_alu_reg_imm_size (code, X86_CMP, MONO_ARCH_IMT_REG, (guint32)(gssize)item->key, sizeof(gpointer));
					else {
						amd64_mov_reg_imm_size (code, MONO_ARCH_IMT_SCRATCH_REG, item->key, sizeof(gpointer));
						amd64_alu_reg_reg (code, X86_CMP, MONO_ARCH_IMT_REG, MONO_ARCH_IMT_SCRATCH_REG);
					}
				}
				item->jmp_code = code;
				amd64_branch8 (code, X86_CC_NE, 0, FALSE);
				if (item->has_target_code) {
					amd64_mov_reg_imm (code, MONO_ARCH_IMT_SCRATCH_REG, item->value.target_code);
					amd64_jump_reg (code, MONO_ARCH_IMT_SCRATCH_REG);
				} else {
					amd64_mov_reg_imm (code, MONO_ARCH_IMT_SCRATCH_REG, & (vtable->vtable [item->value.vtable_slot]));
					amd64_jump_membase (code, MONO_ARCH_IMT_SCRATCH_REG, 0);
				}

				if (fail_case) {
					amd64_patch (item->jmp_code, code);
					amd64_mov_reg_imm (code, MONO_ARCH_IMT_SCRATCH_REG, fail_tramp);
					amd64_jump_reg (code, MONO_ARCH_IMT_SCRATCH_REG);
					item->jmp_code = NULL;
				}
			} else {
				/* enable the commented code to assert on wrong method */
#if 0
				if (amd64_is_imm32 (item->key))
					amd64_alu_reg_imm_size (code, X86_CMP, MONO_ARCH_IMT_REG, (guint32)(gssize)item->key, sizeof(gpointer));
				else {
					amd64_mov_reg_imm (code, MONO_ARCH_IMT_SCRATCH_REG, item->key);
					amd64_alu_reg_reg (code, X86_CMP, MONO_ARCH_IMT_REG, MONO_ARCH_IMT_SCRATCH_REG);
				}
				item->jmp_code = code;
				amd64_branch8 (code, X86_CC_NE, 0, FALSE);
				/* See the comment below about R10 */
				amd64_mov_reg_imm (code, MONO_ARCH_IMT_SCRATCH_REG, & (vtable->vtable [item->value.vtable_slot]));
				amd64_jump_membase (code, MONO_ARCH_IMT_SCRATCH_REG, 0);
				amd64_patch (item->jmp_code, code);
				amd64_breakpoint (code);
				item->jmp_code = NULL;
#else
				/* We're using R10 (MONO_ARCH_IMT_SCRATCH_REG) here because R11 (MONO_ARCH_IMT_REG)
				   needs to be preserved.  R10 needs
				   to be preserved for calls which
				   require a runtime generic context,
				   but interface calls don't. */
				amd64_mov_reg_imm (code, MONO_ARCH_IMT_SCRATCH_REG, & (vtable->vtable [item->value.vtable_slot]));
				amd64_jump_membase (code, MONO_ARCH_IMT_SCRATCH_REG, 0);
#endif
			}
		} else {
			if (amd64_use_imm32 ((gint64)item->key))
				amd64_alu_reg_imm_size (code, X86_CMP, MONO_ARCH_IMT_REG, (guint32)(gssize)item->key, sizeof (target_mgreg_t));
			else {
				amd64_mov_reg_imm_size (code, MONO_ARCH_IMT_SCRATCH_REG, item->key, sizeof (target_mgreg_t));
				amd64_alu_reg_reg (code, X86_CMP, MONO_ARCH_IMT_REG, MONO_ARCH_IMT_SCRATCH_REG);
			}
			item->jmp_code = code;
			if (x86_is_imm8 (imt_branch_distance (imt_entries, i, item->check_target_idx)))
				x86_branch8 (code, X86_CC_GE, 0, FALSE);
			else
				x86_branch32 (code, X86_CC_GE, 0, FALSE);
		}
		g_assertf (code - item->code_target <= item->chunk_size, "%X %X", (guint)(code - item->code_target), (guint)item->chunk_size);
	}
	/* patch the branches to get to the target items */
	for (i = 0; i < count; ++i) {
		MonoIMTCheckItem *item = imt_entries [i];
		if (item->jmp_code) {
			if (item->check_target_idx) {
				amd64_patch (item->jmp_code, imt_entries [item->check_target_idx]->code_target);
			}
		}
	}

	if (!fail_tramp)
		UnlockedAdd (&mono_stats.imt_trampolines_size, code - start);
	g_assert (code - start <= size);
	g_assert_checked (mono_arch_unwindinfo_validate_size (unwind_ops, MONO_TRAMPOLINE_UNWINDINFO_SIZE(0)));

	MONO_PROFILER_RAISE (jit_code_buffer, (start, code - start, MONO_PROFILER_CODE_BUFFER_IMT_TRAMPOLINE, NULL));

	mono_tramp_info_register (mono_tramp_info_create (NULL, start, code - start, NULL, unwind_ops), domain);

	return start;
}

MonoMethod*
mono_arch_find_imt_method (host_mgreg_t *regs, guint8 *code)
{
	return (MonoMethod*)regs [MONO_ARCH_IMT_REG];
}

MonoVTable*
mono_arch_find_static_call_vtable (host_mgreg_t *regs, guint8 *code)
{
	return (MonoVTable*) regs [MONO_ARCH_RGCTX_REG];
}

GSList*
mono_arch_get_cie_program (void)
{
	GSList *l = NULL;

	/* The CIE describes the state before the first instruction, so both ops
	 * sit at code offset 0.  The macros take a code pointer and a buffer base
	 * and subtract them, so pass the same real address twice; subtracting two
	 * null pointers is undefined even though the difference would be 0. */
	guint8 *base = (guint8*) &l;

	mono_add_unwind_op_def_cfa (l, base, base, AMD64_RSP, 8);
	mono_add_unwind_op_offset (l, base, base, AMD64_RIP, -8);

	return l;
}

host_mgreg_t
mono_arch_context_get_int_reg (MonoContext *ctx, int reg)
{
	return ctx->gregs [reg];
}

void
mono_arch_context_set_int_reg (MonoContext *ctx, int reg, host_mgreg_t val)
{
	ctx->gregs [reg] = val;
}

/*
 * mono_arch_get_trampolines:
 *
 *   Return a list of MonoTrampInfo structures describing arch specific trampolines
 * for AOT.
 */
GSList *
mono_arch_get_trampolines (gboolean aot)
{
	return mono_amd64_get_exception_trampolines (aot);
}

/* Soft Debug support */
#ifdef MONO_ARCH_SOFT_DEBUG_SUPPORTED

/*
 * mono_arch_set_breakpoint:
 *
 *   Set a breakpoint at the native code corresponding to JI at NATIVE_OFFSET.
 * The location should contain code emitted by OP_SEQ_POINT.
 */
void
mono_arch_set_breakpoint (MonoJitInfo *ji, guint8 *ip)
{
	guint8 *code = ip;

	if (ji->from_aot) {
		guint32 native_offset = ip - (guint8*)ji->code_start;
		SeqPointInfo *info = mono_arch_get_seq_point_info (mono_domain_get (), (guint8 *)ji->code_start);

		g_assert (info->bp_addrs [native_offset] == 0);
		info->bp_addrs [native_offset] = mini_get_breakpoint_trampoline ();
	} else if (ji->llvm_bp_switch) {
		/*
		 * An LLVM body has no patchable site: its sequence points load the
		 * switch instead, so arming it is a store. The switch covers the
		 * whole body, hence the count - clearing one breakpoint must not
		 * disarm the ones still set elsewhere in the method.
		 */
		ji->llvm_bp_switch->armed ++;
		ji->llvm_bp_switch->tramp = mini_get_breakpoint_trampoline ();
	} else {
		/* ip points to a mov r11, 0 */
		g_assert (code [0] == 0x41);
		g_assert (code [1] == 0xbb);
		amd64_mov_reg_imm (code, AMD64_R11, 1);
	}
}

/*
 * mono_arch_clear_breakpoint:
 *
 *   Clear the breakpoint at IP.
 */
void
mono_arch_clear_breakpoint (MonoJitInfo *ji, guint8 *ip)
{
	guint8 *code = ip;

	if (ji->from_aot) {
		guint32 native_offset = ip - (guint8*)ji->code_start;
		SeqPointInfo *info = mono_arch_get_seq_point_info (mono_domain_get (), (guint8 *)ji->code_start);

		info->bp_addrs [native_offset] = NULL;
	} else if (ji->llvm_bp_switch) {
		if (-- ji->llvm_bp_switch->armed <= 0) {
			ji->llvm_bp_switch->armed = 0;
			ji->llvm_bp_switch->tramp = NULL;
		}
	} else {
		amd64_mov_reg_imm (code, AMD64_R11, 0);
	}
}

/*
 * mono_arch_get_single_step_tramp_addr:
 *
 *   Return the address of the word holding the single step trampoline, which is
 * NULL whenever single stepping is off. Code the LLVM back end emits loads it
 * at every sequence point.
 */
gpointer*
mono_arch_get_single_step_tramp_addr (void)
{
	return &mono_amd64_ss_trampoline;
}

gboolean
mono_arch_is_breakpoint_event (void *info, void *sigctx)
{
	/* We use soft breakpoints on amd64 */
	return FALSE;
}

/*
 * mono_arch_skip_breakpoint:
 *
 *   Modify CTX so the ip is placed after the breakpoint instruction, so when
 * we resume, the instruction is not executed again.
 */
void
mono_arch_skip_breakpoint (MonoContext *ctx, MonoJitInfo *ji)
{
	g_assert_not_reached ();
}
	
/*
 * mono_arch_start_single_stepping:
 *
 *   Start single stepping.
 */
void
mono_arch_start_single_stepping (void)
{
	mono_amd64_ss_trampoline = mini_get_single_step_trampoline ();
}
	
/*
 * mono_arch_stop_single_stepping:
 *
 *   Stop single stepping.
 */
void
mono_arch_stop_single_stepping (void)
{
	mono_amd64_ss_trampoline = NULL;
}

/*
 * mono_arch_is_single_step_event:
 *
 *   Return whenever the machine state in SIGCTX corresponds to a single
 * step event.
 */
gboolean
mono_arch_is_single_step_event (void *info, void *sigctx)
{
	/* We use soft breakpoints on amd64 */
	return FALSE;
}

/*
 * mono_arch_skip_single_step:
 *
 *   Modify CTX so the ip is placed after the single step trigger instruction,
 * we resume, the instruction is not executed again.
 */
void
mono_arch_skip_single_step (MonoContext *ctx)
{
	g_assert_not_reached ();
}

/*
 * mono_arch_create_seq_point_info:
 *
 *   Return a pointer to a data structure which is used by the sequence
 * point implementation in AOTed code.
 */
SeqPointInfo*
mono_arch_get_seq_point_info (MonoDomain *domain, guint8 *code)
{
	SeqPointInfo *info;
	MonoJitInfo *ji;

	// FIXME: Add a free function

	mono_domain_lock (domain);
	info = (SeqPointInfo *)g_hash_table_lookup (domain_jit_info (domain)->arch_seq_points,
								code);
	mono_domain_unlock (domain);

	if (!info) {
		ji = mono_jit_info_table_find (domain, code);
		g_assert (ji);

		// FIXME: Optimize the size
		info = (SeqPointInfo *)g_malloc0 (sizeof (SeqPointInfo) + (ji->code_size * sizeof (gpointer)));

		info->ss_tramp_addr = &mono_amd64_ss_trampoline;

		mono_domain_lock (domain);
		g_hash_table_insert (domain_jit_info (domain)->arch_seq_points,
							 code, info);
		mono_domain_unlock (domain);
	}

	return info;
}

#endif

gpointer
mono_arch_load_function (MonoJitICallId jit_icall_id)
{
	gpointer target = NULL;
	switch (jit_icall_id) {
#undef MONO_AOT_ICALL
#define MONO_AOT_ICALL(x) case MONO_JIT_ICALL_ ## x: target = (gpointer)x; break;
	MONO_AOT_ICALL (mono_amd64_resume_unwind)
	MONO_AOT_ICALL (mono_amd64_start_gsharedvt_call)
	MONO_AOT_ICALL (mono_amd64_throw_corlib_exception)
	MONO_AOT_ICALL (mono_amd64_throw_exception)
	default:
		break;
	}
	return target;
}
