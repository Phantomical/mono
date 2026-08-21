#ifndef __MONO_INTERP_INTERP_ICALL_HPP__
#define __MONO_INTERP_INTERP_ICALL_HPP__

/**
 * \file
 * \brief Calling the runtime's own C functions from interpreted code.
 */

#include "internals.hpp"

namespace mono::interp {

/// Calls the native function ptr with the arguments in sp, and writes the
/// result back over sp [0].
///
/// \param op   the MINT_ICALL_* opcode naming ptr's arity and return kind.
/// \param sig  when non-NULL, converts the result from the native
///             representation to the interpreter one.
///
/// An exception from native code returns through here rather than unwinding
/// past it. A caller whose target can throw therefore sets frame->state.ip
/// before the call and checks the resume state after.
gpointer do_icall_wrapper (InterpFrame *frame, MonoMethodSignature *sig, int op, stackval *sp,
                           gpointer ptr, gboolean save_last_error);

/// Runs one of the handful of methods the runtime implements itself rather
/// than in IL, writing the result over sp.
///
/// Returns what it threw, or NULL.
MonoException *ves_imethod (InterpFrame *frame, MonoMethod *method, MonoMethodSignature *sig,
                            stackval *sp);

/// Builds the buffer a vararg call's arglist reads, from the arguments at sp.
///
/// \param arglist  must have room for the whole variable part plus its cookie.
///
/// The first word holds the call-site signature. Each variable argument
/// follows at the running sum of mono_type_stack_size () over the ones before
/// it. That is what ves_icall_System_ArgIterator_IntGetNextArg () reads back,
/// realigning only on arm and mips. Sizes are not uniform: a float takes four
/// bytes, not a whole slot. An argument written at any other offset shifts
/// every argument behind it, and the iterator misreads them all.
void init_arglist (InterpFrame *frame, MonoMethodSignature *sig, stackval *sp, char *arglist);

} // namespace mono::interp

#endif
