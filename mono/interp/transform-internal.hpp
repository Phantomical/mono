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

namespace mono::interp {

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
