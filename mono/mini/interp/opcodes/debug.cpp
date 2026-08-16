#include "mono/metadata/profiler-private.h"
#include "mono/mini/interp/frame-data.hpp"
#include "mono/mini/interp/interp-internals.h"
#include "mono/mini/interp/interp.hpp"
#include "mono/mini/jit-icalls.h"
#include "mono/mini/debugger-agent.h"
#include "mono/mini/trace.h"
#include <cstring>

namespace mono::interp {

MONO_INTERP_OP_IMPL (MINT_BREAK)
{
	++ip;
	do_debugger_tramp ([&] () { mini_get_dbg_callbacks ()->user_break (); });

	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_BREAKPOINT)
{
	++ip;
	mono_break ();

	MONO_INTERP_DISPATCH ();
}

/*
 * The debugger reaches an interpreted frame through the same trampolines the
 * compiled engine uses, so the last native frame it sees is one of ours. The
 * trampoline address is read once and cached: it does not change, and taking it
 * costs more than the branch that skips it.
 */
MONO_INTERP_OP_IMPL (MINT_SDB_BREAKPOINT)
{
	typedef void (*T) (void);
	static T bp_tramp;

	if (!bp_tramp) {
		void *tramp = mini_get_breakpoint_trampoline ();
		mono_memory_barrier ();
		bp_tramp = (T) tramp;
	}

	/* Add 1 to offset subtraction from interp_frame_get_ip */
	frame->state.ip = ip + 1;

	do_debugger_tramp ([&] () { bp_tramp (); });
	CHECK_RESUME_STATE (context);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_SDB_INTR_LOC)
{
	if (G_UNLIKELY (mono_interp_ss_enabled)) {
		typedef void (*T) (void);
		static T ss_tramp;

		if (!ss_tramp) {
			void *tramp = mini_get_single_step_trampoline ();
			mono_memory_barrier ();
			ss_tramp = (T) tramp;
		}

		/*
		 * Point this at the MINT_SDB_SEQ_POINT that follows, because that is the
		 * address recorded as the sequence point. Add 1 as well, to offset the
		 * subtraction interp_frame_get_ip does.
		 */
		frame->state.ip = ip + 2;

		do_debugger_tramp ([&] () { ss_tramp (); });
		CHECK_RESUME_STATE (context);
	}

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

// The instruction exists so that a sequence point has an address to name.
MONO_INTERP_OP_IMPL (MINT_SDB_SEQ_POINT)
{
	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

/*
 * Both flags name a listener that wants to hear about the entry, and they differ in
 * whether the listener is handed a context. Building one costs an allocation, so it
 * only happens when something asked for it.
 */
MONO_INTERP_OP_IMPL (MINT_PROF_ENTER)
{
	guint16 flag = ip[1];

	MONO_INTERP_OP_ADVANCE ();

	if ((flag & TRACING_FLAG)
	    || ((flag & PROFILING_FLAG) && MONO_PROFILER_ENABLED (method_enter)
	        && (frame->imethod->prof_flags
	            & MONO_PROFILER_CALL_INSTRUMENTATION_ENTER_CONTEXT))) {
		MonoProfilerCallContext *prof_ctx = g_new0 (MonoProfilerCallContext, 1);
		prof_ctx->interp_frame = frame;
		prof_ctx->method = frame->imethod->method;

		if (flag & TRACING_FLAG)
			mono_trace_enter_method (frame->imethod->method, frame->imethod->jinfo, prof_ctx);
		if (flag & PROFILING_FLAG)
			MONO_PROFILER_RAISE (method_enter, (frame->imethod->method, prof_ctx));

		g_free (prof_ctx);
	} else if ((flag & PROFILING_FLAG) && MONO_PROFILER_ENABLED (method_enter)) {
		MONO_PROFILER_RAISE (method_enter, (frame->imethod->method, NULL));
	}

	MONO_INTERP_DISPATCH ();
}

// This is the method's return as well: the listener wants the value, so the return
// happens here rather than at a MINT_RET the transform would have to keep reachable.
MONO_INTERP_OP_IMPL (MINT_PROF_EXIT)
{
	guint16 flag = ip[2];
	int i32 = READ32 (ip + 3);

	if (i32 == -1) {
		// void return, nothing to hand back
	} else if (i32) {
		std::memmove (frame->stack, locals + ip[1], i32);
	} else {
		frame->stack[0] = LOCAL_VAR (ip[1], stackval);
	}

	if ((flag & TRACING_FLAG)
	    || ((flag & PROFILING_FLAG) && MONO_PROFILER_ENABLED (method_leave)
	        && (frame->imethod->prof_flags
	            & MONO_PROFILER_CALL_INSTRUMENTATION_LEAVE_CONTEXT))) {
		MonoProfilerCallContext *prof_ctx = g_new0 (MonoProfilerCallContext, 1);
		prof_ctx->interp_frame = frame;
		prof_ctx->method = frame->imethod->method;
		if (i32 != -1)
			prof_ctx->return_value = frame->stack;

		if (flag & TRACING_FLAG)
			mono_trace_leave_method (frame->imethod->method, frame->imethod->jinfo, prof_ctx);
		if (flag & PROFILING_FLAG)
			MONO_PROFILER_RAISE (method_leave, (frame->imethod->method, prof_ctx));

		g_free (prof_ctx);
	} else if ((flag & PROFILING_FLAG) && MONO_PROFILER_ENABLED (method_enter)) {
		MONO_PROFILER_RAISE (method_leave, (frame->imethod->method, NULL));
	}

	frame_data_allocator_pop (&context->data_stack, frame);

	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_PROF_COVERAGE_STORE)
{
	auto p = (guint32 *) GINT_TO_POINTER (READ64 (ip + 1));
	*p = 1;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
