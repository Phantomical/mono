#ifndef __MONO_INTERP_INTERP_CONTEXT_HPP__
#define __MONO_INTERP_INTERP_CONTEXT_HPP__

/**
 * \file
 * \brief The per-thread state the engine runs on.
 */

#include "interp-internals.h"

/*
 * Publish frame as the one a stack walk reports for this thread. It is set only
 * while the engine is stopped inside a call that can walk its own stack, and
 * cleared again after, so a walk arriving at any other time sees nothing here.
 */
void context_set_current_frame (ThreadContext *context, InterpFrame *frame);

/* Must run before any thread asks for a context. */
void interp_context_init (void);

#endif
