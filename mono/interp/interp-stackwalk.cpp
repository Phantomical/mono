/**
 * \file
 * \brief Walking a thread's interpreter frames from outside the engine.
 */

#include "config.h"

#include "interp-callbacks.hpp"
#include "interp-internals.h"
#include "interp-internals.hpp"
#include "interp-context.hpp"

#include <mono/mini/mini-runtime.h>

namespace mono::interp {

typedef struct {
	InterpFrame *current;
} StackIter;

gpointer
interp_frame_get_ip (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);
	/*
	 * The interpreter keeps the ip of a running frame in a local variable, and writes
	 * state.ip only where the frame stops. A running frame therefore has no ip to
	 * report, and the subtraction below turns a NULL into an address that looks correct.
	 */
	if (!iframe->state.ip)
		return NULL;

	/*
	 * For calls, state.ip points to the instruction following the call, so we need to subtract
	 * in order to get inside the call instruction range. Other instructions that set the IP for
	 * the rest of the runtime to see, like throws and sdb breakpoints, will need to account for
	 * this subtraction that we are doing here.
	 */
	return (gpointer)(iframe->state.ip - 1);
}

/*
 * interp_get_stopped_frame:
 *
 *   Return the frame a thread stopped at inside the interpreter, or NULL if it did
 * not stop there. A stack walk starts an interpreted frame iterator from this.
 *
 * The lmf is the head of the chain the walk itself will follow, and sp is the stack
 * pointer the walk starts from. Both must describe the thread being walked.
 */
gpointer
interp_get_stopped_frame (const MonoJitTlsData *jit_tls, MonoLMF *lmf, gpointer sp)
{
	g_assert (jit_tls);
	ThreadContext *context = (ThreadContext*)jit_tls->interp_context;

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
	 * The interpreter publishes a frame for as long as it runs one, so the frame alone
	 * does not say whether the loop is the innermost thing on this thread. Ask the LMF
	 * chain, and ask it by address. The kind will not do: native code called from the
	 * interpreter pushes its own plain MonoLMF over the interp-exit marker.
	 *
	 * The comparison is against the invocation's native anchor rather than against a
	 * frame, because a frame is not on this stack at all - only the invocation that
	 * runs it is. The stack grows down, so a callout from the loop - a jit call, an
	 * icall, the debugger tramp - lands below that anchor, and an entry the chain
	 * already held belongs to something outer and is above it.
	 *
	 * Only an lmf on the walked thread's stack can be compared this way. Under
	 * --interpreter the head of the chain is off that stack and far below it, and reads
	 * as a callout that did not happen. The stack pointer is the bound: nothing live on
	 * this stack sits below it.
	 *
	 * With no anchor there is no invocation to have stopped in, so say nothing: a walk
	 * that reports one frame less than the truth is the safe way to be wrong.
	 */
	if (!context->handle_mark_count)
		return NULL;

	gpointer anchor = context->handle_marks [context->handle_mark_count - 1].frame;

	if (lmf && (gsize)sp <= (gsize)lmf && (gsize)lmf < (gsize)anchor)
		return NULL;

	return frame;
}

/*
 * The order this frame was entered in, which is what a walk compares frames by.
 */
gsize
interp_frame_ordinal (gpointer interp_frame)
{
	return ((InterpFrame*)interp_frame)->ordinal;
}

/*
 * interp_frame_iter_init:
 *
 *   Initialize an iterator for iterating through interpreted frames.
 */
void
interp_frame_iter_init (MonoInterpStackIter *iter, gpointer interp_exit_data)
{
	StackIter *stack_iter = (StackIter*)iter;

	stack_iter->current = (InterpFrame*)interp_exit_data;
}

/*
 * interp_frame_iter_next:
 *
 *   Fill out FRAME with date for the next interpreter frame.
 */
gboolean
interp_frame_iter_next (MonoInterpStackIter *iter, StackFrameInfo *frame)
{
	StackIter *stack_iter = (StackIter*)iter;
	InterpFrame *iframe = stack_iter->current;

	memset (frame, 0, sizeof (StackFrameInfo));
	/* pinvoke frames doesn't have imethod set */
	while (iframe && !(iframe->imethod && iframe->imethod->code && iframe->imethod->jinfo))
		iframe = iframe->parent;
	if (!iframe)
		return FALSE;

	MonoMethod *method = iframe->imethod->method;
	frame->domain = iframe->imethod->domain;
	frame->interp_frame = iframe;
	frame->method = method;
	frame->actual_method = method;
	if (method && ((method->flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) || (method->iflags & (METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL | METHOD_IMPL_ATTRIBUTE_RUNTIME)))) {
		frame->native_offset = -1;
		frame->type = FRAME_TYPE_MANAGED_TO_NATIVE;
	} else {
		frame->type = FRAME_TYPE_INTERP;
		/* The offset in the interpreter IR. It is -1 if the frame has no ip. */
		gpointer ip = interp_frame_get_ip (iframe);
		frame->native_offset = ip ? (int)((guint8*)ip - (guint8*)iframe->imethod->code) : -1;
		if (!method->wrapper_type || method->wrapper_type == MONO_WRAPPER_DYNAMIC_METHOD)
			frame->managed = TRUE;
	}
	frame->ji = iframe->imethod->jinfo;
	frame->frame_addr = iframe;

	stack_iter->current = iframe->parent;

	return TRUE;
}

} // namespace mono::interp

/* Outside the namespace: interp-internals.h declares these for C, so the
 * definitions have to match the C linkage that gives them. */

using namespace mono::interp;

gpointer
mono_interp_invocation_anchor (ThreadContext *context)
{
	if (!context->handle_mark_count)
		return NULL;

	return context->handle_marks [context->handle_mark_count - 1].frame;
}
