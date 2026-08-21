#ifndef __MONO_INTERP_INTERP_CONTEXT_HPP__
#define __MONO_INTERP_INTERP_CONTEXT_HPP__

/**
 * \file
 * \brief The per-thread state the engine runs on.
 */

#include "internals.hpp"

namespace mono::interp {

/// Sets frame as context's current frame. See ThreadContext::current_frame for
/// what a stack walk reads from it.
void context_set_current_frame (ThreadContext *context, InterpFrame *frame);

/// Must run before any thread asks for a context.
void interp_context_init (void);

} // namespace mono::interp

#endif
