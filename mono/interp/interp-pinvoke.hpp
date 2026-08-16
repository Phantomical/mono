#ifndef __MONO_INTERP_INTERP_PINVOKE_HPP__
#define __MONO_INTERP_INTERP_PINVOKE_HPP__

/**
 * \file
 * \brief Calling out of the interpreter into native code.
 */

#include "interp-internals.h"

/*
 * Calls the native function addr with the arguments in sp, marshalled the way sig
 * describes, and writes the result back over sp. imethod is the pinvoke method the
 * wrapper making this call stands for, and NULL at a calli that is not inside a
 * managed-to-native wrapper. cache must be stable per-call-site storage, because
 * the wasm build caches the entry trampoline in it.
 *
 * An exception from native code returns through here rather than unwinding past
 * it. A caller whose target can throw therefore sets parent_frame->state.ip before
 * the call and checks the resume state after.
 */
gpointer ves_pinvoke_method (InterpMethod *imethod, MonoMethodSignature *sig, MonoFuncV addr,
                             ThreadContext *context, InterpFrame *parent_frame, stackval *sp,
                             gboolean save_last_error, gpointer *cache);

#endif
