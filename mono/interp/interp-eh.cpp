/**
 * \file
 * \brief Handing a frame back to the engine after the runtime unwound to it.
 */

#include "config.h"

#include "interp-callbacks.hpp"
#include "interp-internals.h"
#include "interp-internals.hpp"
#include "interp-context.hpp"

#include <mono/mini/mini-runtime.h>

/*
 * interp_release_abandoned_handles:
 *
 *   Give back the handles held by the interpreter frames an exception resume is
 * about to skip.
 */
void
interp_release_abandoned_handles (MonoJitTlsData *jit_tls, gpointer resume_sp)
{
	g_assert (jit_tls);
	ThreadContext *context = (ThreadContext*)jit_tls->interp_context;
	if (!context)
		return;

	/*
	 * Resuming restores the stack pointer over every frame below the one it
	 * resumes into, so those interp_exec_method () invocations never reach
	 * their own HANDLE_FUNCTION_RETURN. Entries are in stack order, so the
	 * ones being skipped are a suffix, and restoring the outermost of them
	 * hands back every handle above it at once.
	 */
	int first = context->handle_mark_count;

	while (first > 0 && context->handle_marks [first - 1].frame < resume_sp)
		first--;

	gboolean dropped = first < context->handle_mark_count;
	guchar *watermark = NULL;
	gsize first_ordinal = 0;

	if (dropped) {
		watermark = context->handle_marks [first].frame_watermark;
		first_ordinal = context->handle_marks [first].first_ordinal;

		mono_stack_mark_pop (mono_thread_info_current (), &context->handle_marks [first].mark);
		context->handle_mark_count = first;
	}

	/*
	 * The skipped invocations do not restore anything else either. Their frames go
	 * back here, because nothing on a frame the resume jumps over runs again, and the
	 * marker goes with them: nothing here knows which frame of the invocation below is
	 * current, so say nothing rather than name a dead one.
	 */
	if (dropped) {
		context->frame_stack_pointer = watermark;

		if (context->current_frame && context->current_frame->ordinal >= first_ordinal)
			context_set_current_frame (context, NULL);
	}
}

/*
 * interp_set_resume_state:
 *
 *   Set the state the interpeter will continue to execute from after execution returns to the interpreter.
 */
void
interp_set_resume_state (MonoJitTlsData *jit_tls, MonoObject *ex, MonoJitExceptionInfo *ei, MonoInterpFrameHandle interp_frame, gpointer handler_ip)
{
	ThreadContext *context;

	g_assert (jit_tls);
	context = (ThreadContext*)jit_tls->interp_context;
	g_assert (context);

	context->has_resume_state = TRUE;
	context->handler_frame = (InterpFrame*)interp_frame;
	context->handler_ei = ei;
	if (context->exc_gchandle)
		mono_gchandle_free_internal (context->exc_gchandle);
	context->exc_gchandle = mono_gchandle_new_internal ((MonoObject*)ex, FALSE);
	/* Ditto */
	if (ei)
		*(MonoObject**)(frame_locals (context->handler_frame) + ei->exvar_offset) = ex;
	context->handler_ip = (const guint16*)handler_ip;
}

void
interp_get_resume_state (const MonoJitTlsData *jit_tls, gboolean *has_resume_state, MonoInterpFrameHandle *interp_frame, gpointer *handler_ip)
{
	g_assert (jit_tls);
	ThreadContext *context = (ThreadContext*)jit_tls->interp_context;

	*has_resume_state = context ? context->has_resume_state : FALSE;
	if (!*has_resume_state)
		return;

	*interp_frame = context->handler_frame;
	*handler_ip = (gpointer)context->handler_ip;
}
