#ifndef __MONO_INTERP_INTERP_TRACE_HPP__
#define __MONO_INTERP_INTERP_TRACE_HPP__

/**
 * \file
 * \brief Printing a method's frames and opcodes as it executes.
 */

#include "config.h"
#include "internals.hpp"

namespace mono::interp {

#ifdef ENABLE_INTERP_TRACE

/*
 * Whether MONO_INTERP_TRACE names this method. Asked once per InterpMethod, so a
 * frame reads the answer off the method rather than matching a name again.
 */
gboolean trace_wants_method (MonoMethod *method);

void trace_enter (ThreadContext *context, InterpFrame *frame);
void trace_leave (ThreadContext *context, InterpFrame *frame);

#define MONO_INTERP_TRACE_ENTER(context, frame)         \
	do {                                                \
		if (G_UNLIKELY ((frame)->imethod->tracing))     \
			trace_enter ((context), (frame));           \
	} while (0)

#define MONO_INTERP_TRACE_LEAVE(context, frame)         \
	do {                                                \
		if (G_UNLIKELY ((frame)->imethod->tracing))     \
			trace_leave ((context), (frame));           \
	} while (0)

#else

#define MONO_INTERP_TRACE_ENTER(context, frame) \
	do {                                        \
	} while (0)
#define MONO_INTERP_TRACE_LEAVE(context, frame) \
	do {                                        \
	} while (0)

#endif

} // namespace mono::interp

#endif
