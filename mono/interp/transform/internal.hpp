#ifndef __MONO_INTERP_TRANSFORM_INTERNAL_HPP__
#define __MONO_INTERP_TRANSFORM_INTERNAL_HPP__

/**
 * \file
 * \brief What the transform's own files share, and nothing outside them uses.
 *
 * TransformData's members are declared in transform.hpp, which the unit tests
 * read. This holds the free helpers instead - the ones that answer a question
 * about an instruction or a block without needing the transform.
 */

#include "transform.hpp"

#include <optional>

#if SIZEOF_VOID_P == 8
#define MINT_MOV_P MINT_MOV_8
#else
#define MINT_MOV_P MINT_MOV_4
#endif

/*
 * A stack slot's type, set together with the flags and class that go with it so
 * a site cannot set one and forget the others.
 */
#define SET_SIMPLE_TYPE(s, ty)                 \
	do {                                       \
		g_assert (ty != StackType::VT);        \
		g_assert ((s)->type != StackType::VT); \
		(s)->type = (ty);                      \
		(s)->flags = 0;                        \
		(s)->klass = NULL;                     \
	} while (0)

#define SET_TYPE(s, ty, k)                     \
	do {                                       \
		g_assert (ty != StackType::VT);        \
		g_assert ((s)->type != StackType::VT); \
		(s)->type = (ty);                      \
		(s)->flags = 0;                        \
		(s)->klass = k;                        \
	} while (0)

#define BARRIER_IF_VOLATILE(kind)              \
	do {                                       \
		if (volatile_) {                       \
			interp_emit_memory_barrier (kind); \
			volatile_ = FALSE;                 \
		}                                      \
	} while (0)
#define INLINE_FAILURE    \
	do {                  \
		if (inlining)     \
			return FALSE; \
	} while (0)

/*
 * A guard on the eval stack depth, for use inside a TransformData member. It
 * warns rather than refusing: a short stack here means the IL was already bad.
 */
#define CHECK_STACK(n)                                                                \
	do {                                                                              \
		int stack_size = sp - stack;                                                  \
		if (stack_size < (n))                                                         \
			g_warning ("%s.%s: not enough values (%d < %d) on stack at %04x",         \
			           m_class_get_name (method->klass), method->name, stack_size, n, \
			           ip - il_code);                                                 \
	} while (0)

/*
 * Writing a wide immediate into the instruction stream, which is an array of
 * guint16 and so does not line a 32- or 64-bit value up on its own.
 */
#if NO_UNALIGNED_ACCESS
#define WRITE32(ip, v)                        \
	do {                                      \
		*(ip) = *(guint16 *) (v);             \
		*((ip) + 1) = *((guint16 *) (v) + 1); \
		(ip) += 2;                            \
	} while (0)

#define WRITE32_INS(ins, index, v)                       \
	do {                                                 \
		(ins)->data[index] = *(guint16 *) (v);           \
		(ins)->data[index + 1] = *((guint16 *) (v) + 1); \
	} while (0)

#define WRITE64(ins, v)                        \
	do {                                       \
		*((ins) + 0) = *((guint16 *) (v) + 0); \
		*((ins) + 1) = *((guint16 *) (v) + 1); \
		*((ins) + 2) = *((guint16 *) (v) + 2); \
		*((ins) + 3) = *((guint16 *) (v) + 3); \
	} while (0)

#define WRITE64_INS(ins, index, v)                       \
	do {                                                 \
		(ins)->data[index] = *(guint16 *) (v);           \
		(ins)->data[index + 1] = *((guint16 *) (v) + 1); \
		(ins)->data[index + 2] = *((guint16 *) (v) + 2); \
		(ins)->data[index + 3] = *((guint16 *) (v) + 3); \
	} while (0)
#else
#define WRITE32(ip, v)                        \
	do {                                      \
		*(guint32 *) (ip) = *(guint32 *) (v); \
		(ip) += 2;                            \
	} while (0)
#define WRITE32_INS(ins, index, v)                             \
	do {                                                       \
		*(guint32 *) (&(ins)->data[index]) = *(guint32 *) (v); \
	} while (0)

#define WRITE64(ip, v)                        \
	do {                                      \
		*(guint64 *) (ip) = *(guint64 *) (v); \
		(ip) += 4;                            \
	} while (0)
#define WRITE64_INS(ins, index, v)                             \
	do {                                                       \
		*(guint64 *) (&(ins)->data[index]) = *(guint64 *) (v); \
	} while (0)

#endif

/*
 * An instruction's registers, set through macros sized to how many an opcode
 * takes, so a site cannot leave one unset.
 */
#define interp_ins_set_dreg(ins, dr) \
	do {                             \
		ins->dreg = dr;              \
	} while (0)

#define interp_ins_set_sreg(ins, s1) \
	do {                             \
		ins->sregs[0] = s1;          \
	} while (0)

#define interp_ins_set_sregs2(ins, s1, s2) \
	do {                                   \
		ins->sregs[0] = s1;                \
		ins->sregs[1] = s2;                \
	} while (0)

#define interp_ins_set_sregs3(ins, s1, s2, s3) \
	do {                                       \
		ins->sregs[0] = s1;                    \
		ins->sregs[1] = s2;                    \
		ins->sregs[2] = s3;                    \
	} while (0)

namespace mono::interp {

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

/// Returns the wrapper the interpreter has to call target_method through, or
/// target_method unchanged. A PInvoke and an internal call get a native
/// wrapper, a synchronized method a synchronized wrapper.
MonoMethod *interp_transform_internal_calls (MonoMethod *method, MonoMethod *target_method,
                                             MonoMethodSignature *csignature, gboolean is_virtual);

/// Which of the magic-int families (nint, nuint, nfloat) this class is, or -1.
int mono_class_get_magic_index (MonoClass *k);
MonoClass *magic_class_of (MonoType *type);

/// The MINT_LDIND that loads a value of this storage type.
int interp_get_ldind_for_mt (MintType mt);

/// The MINT_ICALL that matches this signature's shape, or MINT_NIY.
int interp_icall_op_for_sig (MonoMethodSignature *sig);

/// The method a token names, with the generic context applied.
MonoMethod *interp_get_method (MonoMethod *method, guint32 token, MonoImage *image,
                               MonoGenericContext *generic_context, MonoError *error);

/// The field a token names, and the class that declares it.
MonoClassField *interp_field_from_token (MonoMethod *method, guint32 token, MonoClass **klass,
                                         MonoGenericContext *generic_context, MonoError *error);

/// The header of a method the transform can inline, or null when it has none.
MonoMethodHeader *interp_method_get_header (MonoMethod *method, MonoError *error);

/// R4 or R8 for a floating-point type. Empty for anything else, which
/// coerce_fp () leaves alone.
std::optional<StackType> fp_stack_type (MonoType *type);

/// Whether a value type holds any reference the GC has to see.
gboolean type_has_references (MonoType *type);

/// Turns an instruction into a MINT_NOP in place. It stays linked, so a walk
/// standing on it can still advance.
void interp_clear_ins (InterpInst *ins);

/// The instruction before ins that is not a MINT_NOP, or null at the start of
/// the block.
InterpInst *interp_prev_ins (InterpInst *ins);

/// Drops the edge from `from` to `to`, however many times it is recorded.
void interp_unlink_bblocks (InterpBasicBlock *from, InterpBasicBlock *to);

/// The MINT_MOV that moves a value of this storage type. `needs_sext` asks for
/// the member that sign-extends rather than the one that copies a slot.
int get_mov_for_type (MintType mt, gboolean needs_sext);

/// Whether ins loads a constant, and what it loads.
gboolean interp_ins_is_ldc (InterpInst *ins);
gint32 interp_get_const_from_ldc_i4 (InterpInst *ins);

/// Prints one instruction, for the transform's own tracing.
void dump_interp_inst (InterpInst *ins);

/// Prints a run of emitted bytecode to out.
void dump_interp_code (FILE *out, const guint16 *start, const guint16 *end);

/// The operands of one instruction, as a string the caller frees. `ins` can be
/// null, which means the data belongs to an instruction already emitted.
char *dump_interp_ins_data (InterpInst *ins, gint32 ins_offset, const guint16 *data,
                            guint16 opcode);

} // namespace mono::interp

#endif /* __MONO_INTERP_TRANSFORM_INTERNAL_HPP__ */
