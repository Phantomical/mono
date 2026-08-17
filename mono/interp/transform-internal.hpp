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

#if SIZEOF_VOID_P == 8
#define MINT_MOV_P MINT_MOV_8
#else
#define MINT_MOV_P MINT_MOV_4
#endif

/*
 * A stack slot's type, set together with the flags and class that go with it so
 * a site cannot set one and forget the others.
 */
#define SET_SIMPLE_TYPE(s, ty)                                                                     \
	do {                                                                                       \
		g_assert (ty != StackType::VT);                                                    \
		g_assert ((s)->type != StackType::VT);                                             \
		(s)->type = (ty);                                                                  \
		(s)->flags = 0;                                                                    \
		(s)->klass = NULL;                                                                 \
	} while (0)

#define SET_TYPE(s, ty, k)                                                                         \
	do {                                                                                       \
		g_assert (ty != StackType::VT);                                                    \
		g_assert ((s)->type != StackType::VT);                                             \
		(s)->type = (ty);                                                                  \
		(s)->flags = 0;                                                                    \
		(s)->klass = k;                                                                    \
	} while (0)

/*
 * An instruction's registers, written through a macro so a site that sets one
 * cannot quietly set the wrong number of them.
 */
#define interp_ins_set_dreg(ins, dr)                                                               \
	do {                                                                                       \
		ins->dreg = dr;                                                                    \
	} while (0)

#define interp_ins_set_sreg(ins, s1)                                                               \
	do {                                                                                       \
		ins->sregs [0] = s1;                                                               \
	} while (0)

#define interp_ins_set_sregs2(ins, s1, s2)                                                         \
	do {                                                                                       \
		ins->sregs [0] = s1;                                                               \
		ins->sregs [1] = s2;                                                               \
	} while (0)

#define interp_ins_set_sregs3(ins, s1, s2, s3)                                                     \
	do {                                                                                       \
		ins->sregs [0] = s1;                                                               \
		ins->sregs [1] = s2;                                                               \
		ins->sregs [2] = s3;                                                               \
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

/// The method an InternalCall or a JIT-intrinsic attribute redirects a call to,
/// or the target unchanged.
MonoMethod *interp_transform_internal_calls (MonoMethod *method, MonoMethod *target_method,
                                            MonoMethodSignature *csignature, gboolean is_virtual);

/// Which of the magic-int families (nint, nuint, nfloat) this class is, or -1.
int mono_class_get_magic_index (MonoClass *k);

/// The MINT_LDIND that loads a value of this storage type.
int interp_get_ldind_for_mt (MintType mt);

/// The MINT_ICALL that matches this signature's shape, or MINT_NIY.
int interp_icall_op_for_sig (MonoMethodSignature *sig);

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

} // namespace mono::interp

#endif /* __MONO_INTERP_TRANSFORM_INTERNAL_HPP__ */
