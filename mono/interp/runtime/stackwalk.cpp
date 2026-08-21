/**
 * \file
 * \brief Walking a thread's interpreter frames from outside the engine.
 */

#include "config.h"

#include "callbacks.hpp"
#include "internals.hpp"
#include "context.hpp"

#include <mono/mini/mini-runtime.h>

namespace mono::interp {

struct StackIter {
	InterpFrame *current;
};

gpointer
interp_frame_get_ip (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = static_cast<InterpFrame *> (frame);

	g_assert (iframe->imethod);
	/*
	 * The interpreter keeps the ip of a running frame in a local variable, and writes
	 * state.ip only where the frame stops. A running frame therefore has no ip to
	 * report, and the subtraction below turns a NULL into an address that looks correct.
	 */
	if (!iframe->state.ip)
		return NULL;

	/*
	 * For a call, state.ip points to the instruction after the call, so we
	 * subtract one to land inside the call instruction. Any other site that
	 * publishes state.ip must add one to offset this subtraction. A throw and
	 * an sdb breakpoint do this.
	 */
	return (gpointer) (iframe->state.ip - 1);
}

/**
 * Returns the frame a thread stopped at inside the interpreter, or NULL if it
 * did not stop there. A stack walk starts an interpreted frame iterator from
 * this.
 *
 * lmf is the head of the chain the walk itself follows, and sp is the stack
 * pointer the walk starts from. Both must describe the thread being walked.
 */
gpointer
interp_get_stopped_frame (const MonoJitTlsData *jit_tls, MonoLMF *lmf, gpointer sp)
{
	g_assert (jit_tls);
	ThreadContext *context = static_cast<ThreadContext *> (jit_tls->interp_context);

	if (!context)
		return NULL;

	/* A safepoint names the frame exactly, so it wins. */
	if (context->safepoint_frame)
		return context->safepoint_frame;

	/* Read once. A sampling signal can arrive between two reads of this field. */
	InterpFrame *frame = context->current_frame;

	if (!frame)
		return NULL;

	/*
	 * The interpreter publishes a frame for as long as it runs one. The frame
	 * alone does not say whether the loop is the innermost thing on this
	 * thread. We check the LMF chain instead, by address. The kind will not
	 * do: native code called from the interpreter pushes its own plain
	 * MonoLMF over the interp-exit marker.
	 *
	 * We compare against the invocation's native anchor, not a frame. A frame
	 * is never on this stack at all - only the invocation that runs it is.
	 * The stack grows down, so a callout from the loop - a jit call, an
	 * icall, the debugger tramp - lands below that anchor. An entry the
	 * chain already held belongs to something outer, and sits above it.
	 *
	 * Only an lmf on the walked thread's stack can be compared this way. Under
	 * --interpreter the head of the chain is off that stack and far below it, and reads
	 * as a callout that did not happen. The stack pointer is the bound: nothing live on
	 * this stack sits below it.
	 *
	 * With no anchor there is no invocation to have stopped in, so we report
	 * nothing. A walk that reports one frame less than the truth is the safe
	 * way to be wrong.
	 */
	if (!context->handle_mark_count)
		return NULL;

	gpointer anchor = context->handle_marks[context->handle_mark_count - 1].frame;

	if (lmf && (gsize) sp <= (gsize) lmf && (gsize) lmf < (gsize) anchor)
		return NULL;

	return frame;
}

/// Returns the order this frame was entered in, which a walk uses to compare frames.
gsize
interp_frame_ordinal (gpointer interp_frame)
{
	return (static_cast<InterpFrame *> (interp_frame))->ordinal;
}

/// Starts iter walking interpreted frames from interp_exit_data.
void
interp_frame_iter_init (MonoInterpStackIter *iter, gpointer interp_exit_data)
{
	StackIter *stack_iter = reinterpret_cast<StackIter *> (iter);

	stack_iter->current = static_cast<InterpFrame *> (interp_exit_data);
}

/// Advances iter and fills frame with the next interpreted frame. Returns
/// false once no frames remain.
gboolean
interp_frame_iter_next (MonoInterpStackIter *iter, StackFrameInfo *frame)
{
	StackIter *stack_iter = reinterpret_cast<StackIter *> (iter);
	InterpFrame *iframe = stack_iter->current;

	memset (frame, 0, sizeof (StackFrameInfo));
	/* A pinvoke frame has no imethod set. */
	while (iframe && !(iframe->imethod && iframe->imethod->code && iframe->imethod->jinfo))
		iframe = iframe->parent;
	if (!iframe)
		return FALSE;

	MonoMethod *method = iframe->imethod->method;
	frame->domain = iframe->imethod->domain;
	frame->interp_frame = iframe;
	frame->method = method;
	frame->actual_method = method;
	if (method
	    && ((method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL)
	        || (method->iflags
	            & (METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL | METHOD_IMPL_ATTRIBUTE_RUNTIME)))) {
		frame->native_offset = -1;
		frame->type = FRAME_TYPE_MANAGED_TO_NATIVE;
	} else {
		frame->type = FRAME_TYPE_INTERP;
		/* The offset into the interpreter bytecode. It is -1 if the frame has no ip. */
		gpointer ip = interp_frame_get_ip (iframe);
		frame->native_offset = ip ? (int) (static_cast<guint8 *> (ip)
		                                   - reinterpret_cast<guint8 *> (iframe->imethod->code))
		                          : -1;
		if (!method->wrapper_type || method->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD)
			frame->managed = TRUE;
	}
	frame->ji = iframe->imethod->jinfo;
	frame->frame_addr = iframe;

	stack_iter->current = iframe->parent;

	return TRUE;
}

} // namespace mono::interp

/* Outside the namespace, because internals.hpp declares them there. */

using namespace mono::interp;

gpointer
mono_interp_invocation_anchor (ThreadContext *context)
{
	if (!context->handle_mark_count)
		return NULL;

	return context->handle_marks[context->handle_mark_count - 1].frame;
}
