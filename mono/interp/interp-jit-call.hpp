#ifndef __MONO_INTERP_INTERP_JIT_CALL_HPP__
#define __MONO_INTERP_INTERP_JIT_CALL_HPP__

/**
 * \file
 * \brief Calling a method body the compiled tier has already built.
 */

#include "interp-internals.hpp"
#include "mono/utils/mono-error-internals.h"

namespace mono::interp {

/*
 * Calls the compiled body of rmethod with the arguments at sp, and writes its
 * result back over sp. What it threw comes back in error, because the exception
 * has to be raised from the interpreted frame rather than from here.
 */
void do_jit_call (stackval *sp, InterpFrame *frame, InterpMethod *rmethod, MonoError *error);

} // namespace mono::interp

#endif
