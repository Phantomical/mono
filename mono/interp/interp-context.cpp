/**
 * \file
 * \brief The per-thread state the engine runs on, and its GC roots.
 */

#include "config.h"

#include "interp-callbacks.hpp"
#include "interp-internals.h"
#include "interp-internals.hpp"
#include "frame-data.hpp"
#include "interp-context.hpp"

#include <mono/metadata/gc-internals.h>
#include <mono/mini/mini-runtime.h>
#include <mono/utils/mono-tls-inline.h>

namespace mono::interp {

static MonoNativeTlsKey thread_context_id;

static void
set_context (ThreadContext *context)
{
	mono_native_tls_set_value (thread_context_id, context);

	if (!context)
		return;

	MonoJitTlsData *jit_tls = mono_tls_get_jit_tls ();
	g_assertf (jit_tls, "ThreadContext needs initialized JIT TLS");

	/* jit_tls assumes ownership of 'context' */
	jit_tls->interp_context = context;
}

void
interp_free_context (gpointer ctx)
{
	ThreadContext *context = (ThreadContext*)ctx;

	ThreadContext *current_context = (ThreadContext *) mono_native_tls_get_value (thread_context_id);
	/* at thread exit, we can be called from the JIT TLS key destructor with current_context == NULL */
	if (current_context != NULL) {
		/* check that the context we're freeing is the current one before overwriting TLS */
		g_assert (context == current_context);
		set_context (NULL);
	}

	context->safepoint_frame = NULL;

	mono_vfree (context->stack_start, INTERP_STACK_SIZE, MONO_MEM_ACCOUNT_INTERP_STACK);
	mono_vfree (context->frame_stack_start, INTERP_FRAME_STACK_SIZE, MONO_MEM_ACCOUNT_INTERP_STACK);
	/* Prevent interp_mark_stack from trying to scan the data_stack, before freeing it */
	context->stack_start = NULL;
	context->frame_stack_start = NULL;
	mono_compiler_barrier ();
	frame_data_allocator_free (&context->data_stack);
	g_free (context->handle_marks);
	g_free (context);
}

/*
 * Record the frame that the interpreter loop runs, so that a walk of this thread's stack
 * can find its interpreted frames.
 *
 * A call publishes the callee only after the callee is ready to run. A return publishes
 * the caller before anything retires the callee. Both orders name a frame that is live.
 * A walk that arrives in one of those windows reports one frame less than the truth, and
 * that is the safe direction to be wrong in.
 */
void
context_set_current_frame (ThreadContext *context, InterpFrame *frame)
{
	context->current_frame = frame;
}

void
interp_mark_stack (gpointer thread_data, GcScanFunc func, gpointer gc_data, gboolean precise)
{
	MonoThreadInfo *info = (MonoThreadInfo*)thread_data;

	if (!mono_use_interpreter)
		return;
	if (precise)
		return;

	/*
	 * We explicitly mark the frames instead of registering the stack fragments as GC roots, so
	 * we have to process less data and avoid false pinning from data which is above 'pos'.
	 *
	 * The stack frame handling code uses compiler write barriers only, but the calling code
	 * in sgen-mono.c already did a mono_memory_barrier_process_wide () so we can
	 * process these data structures normally.
	 */
	MonoJitTlsData *jit_tls = (MonoJitTlsData *)info->tls [TLS_KEY_JIT_TLS];
	if (!jit_tls)
		return;

	ThreadContext *context = (ThreadContext*)jit_tls->interp_context;
	if (!context || !context->stack_start)
		return;

	// FIXME: Scan the whole area with 1 call
	for (gpointer *p = (gpointer*)context->stack_start; p < (gpointer*)context->stack_pointer; p++)
		func (p, gc_data);

	/*
	 * Frames the interpreter made for its own calls. The rest live on the native
	 * stack, which is scanned conservatively already.
	 */
	if (context->frame_stack_start) {
		for (gpointer *p = (gpointer*)context->frame_stack_start; p < (gpointer*)context->frame_stack_pointer; p++)
			func (p, gc_data);
	}

	FrameDataFragment *frag;
	for (frag = context->data_stack.first; frag; frag = frag->next) {
		// FIXME: Scan the whole area with 1 call
		for (gpointer *p = (gpointer*)&frag->data; p < (gpointer*)frag->pos; ++p)
			func (p, gc_data);
		if (frag == context->data_stack.current)
			break;
	}
}

/* Allocates the key the per-thread context hangs off, and clears it for the thread
 * that runs this. */
void
interp_context_init (void)
{
	mono_native_tls_alloc (&thread_context_id, NULL);
	set_context (NULL);
}

} // namespace mono::interp

/* Outside the namespace: interp-internals.h declares these for C, so the
 * definitions have to match the C linkage that gives them. */

using namespace mono::interp;

ThreadContext *
mono_interp_get_context (void)
{
	ThreadContext *context = (ThreadContext *) mono_native_tls_get_value (thread_context_id);
	if (context == NULL) {
		context = g_new0 (ThreadContext, 1);
		context->stack_start = (guchar*)mono_valloc (0, INTERP_STACK_SIZE, MONO_MMAP_READ | MONO_MMAP_WRITE, MONO_MEM_ACCOUNT_INTERP_STACK);
		context->stack_pointer = context->stack_start;

		context->frame_stack_start = (guchar*)mono_valloc (0, INTERP_FRAME_STACK_SIZE, MONO_MMAP_READ | MONO_MMAP_WRITE, MONO_MEM_ACCOUNT_INTERP_STACK);
		context->frame_stack_pointer = context->frame_stack_start;

		frame_data_allocator_init (&context->data_stack, 8192);
		/* Make sure all data is initialized before publishing the context */
		mono_compiler_barrier ();
		set_context (context);
	}
	return context;
}

int
mono_interp_push_handle_mark (ThreadContext *context, HandleStackMark *mark, InterpFrame *frame)
{
	int depth = context->handle_mark_count;

	if (G_UNLIKELY (depth == context->handle_mark_capacity)) {
		context->handle_mark_capacity = context->handle_mark_capacity ? context->handle_mark_capacity * 2 : 32;
		context->handle_marks = g_renew (InterpHandleMark, context->handle_marks, context->handle_mark_capacity);
	}

	context->handle_marks [depth].mark = *mark;
	/* Compared against the stack pointer a resume restores, so it has to be in the frame. */
	context->handle_marks [depth].frame = (gpointer)mark;
	context->handle_marks [depth].frame_watermark = context->frame_stack_pointer;
	context->handle_marks [depth].first_ordinal = frame->ordinal;
	context->handle_mark_count = depth + 1;

	return depth;
}

void
mono_interp_error_cleanup (MonoError* error)
{
	mono_error_cleanup (error); /* FIXME: don't swallow the error */
	error_init_reuse (error); // one instruction, so this function is good inline candidate
}
