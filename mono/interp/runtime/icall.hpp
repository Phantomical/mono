#ifndef __MONO_INTERP_INTERP_ICALL_HPP__
#define __MONO_INTERP_INTERP_ICALL_HPP__

/**
 * \file
 * \brief Calling the runtime's own C functions from interpreted code.
 */

#include "internals.hpp"

namespace mono::interp {

/*
 * Calls the native function ptr with the arguments in sp, and writes the result
 * back over sp [0]. op is the MINT_ICALL_* opcode naming its arity and return
 * kind. A non-NULL sig converts the result from the native representation to the
 * interpreter one.
 *
 * An exception from native code returns through here rather than unwinding past
 * it. A caller whose target can throw therefore sets frame->state.ip before the
 * call and checks the resume state after.
 */
gpointer do_icall_wrapper (InterpFrame *frame, MonoMethodSignature *sig, int op, stackval *sp,
                           gpointer ptr, gboolean save_last_error);

/*
 * Runs one of the handful of methods the runtime implements itself rather than in
 * IL, writing the result over sp. Returns what it threw, or NULL.
 */
MonoException *ves_imethod (InterpFrame *frame, MonoMethod *method, MonoMethodSignature *sig,
                            stackval *sp);

/*
 * Builds the buffer a vararg call's arglist reads, from the arguments at sp.
 * arglist must have room for the whole variable part plus its cookie.
 */
void init_arglist (InterpFrame *frame, MonoMethodSignature *sig, stackval *sp, char *arglist);

} // namespace mono::interp

#endif
