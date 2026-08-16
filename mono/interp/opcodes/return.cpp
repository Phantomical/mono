#include "config.h"

/**
 * \file
 * \brief Leaving a frame: the return opcodes and the tail-position jmp.
 */

#include "mintops.h"
#include "mono/interp/interp-frame.hpp"
#include "mono/interp/interp-internals.h"
#include "mono/interp/interp-method.hpp"
#include "mono/interp/interp-stackval.hpp"
#include "mono/interp/interp-trace.hpp"
#include "mono/interp/interp.hpp"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/profiler.h"
#include <cstring>

namespace mono::interp {

MONO_INTERP_ENTRY (exec_exit_frame, exit_frame);

MONO_ALWAYS_INLINE InterpState::OpFunc
InterpState::exit_frame ()
{
	g_assert_checked (frame->imethod);

	MONO_INTERP_TRACE_LEAVE (context, frame);

	if (frame->parent && frame->parent->state.ip) {
		// return to the main loop after a non-recursive interpreter call
		g_assert_checked (frame->stack);
		// A suspended parent means call () made this frame, so it is the top of the
		// frame stack and nothing above it is live.
		g_assert_checked ((guchar *) frame >= context->frame_stack_start
		                  && (guchar *) (frame + 1) <= context->frame_stack_pointer);
		context->frame_stack_pointer = (guchar *) frame;
		frame = frame->parent;
		context->current_frame = frame;
		context->stack_pointer = (guchar *) frame->stack + frame->imethod->alloca_size;
		LOAD_INTERP_STATE (frame);
		CHECK_RESUME_STATE (context);

		MONO_INTERP_DISPATCH ();
	}

	return &exec_exit;
}

MONO_INTERP_OP_IMPL (MINT_RET)
{
	frame->stack[0] = LOCAL_VAR (ip[1], stackval);
	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_RET_VOID)
{
	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_RET_VT)
{
	std::memmove (frame->stack, &LOCAL_VAR (ip[1], char), ip[2]);
	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_RET_LOCALLOC)
{
	frame->stack[0] = LOCAL_VAR (ip[1], stackval);
	frame_data_allocator_pop (&context->data_stack, frame);
	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_RET_VOID_LOCALLOC)
{
	frame_data_allocator_pop (&context->data_stack, frame);
	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_RET_VT_LOCALLOC)
{
	std::memmove (frame->stack, &LOCAL_VAR (ip[1], char), ip[2]);
	frame_data_allocator_pop (&context->data_stack, frame);
	return &exec_exit_frame;
}

MONO_INTERP_OP_IMPL (MINT_JMP)
{
	auto new_method = (InterpMethod *) frame->imethod->data_items[ip[1]];

	if (frame->imethod->prof_flags & MONO_PROFILER_CALL_INSTRUMENTATION_TAIL_CALL)
		MONO_PROFILER_RAISE (method_tail_call, (frame->imethod->method, new_method->method));

	if (G_UNLIKELY (!new_method->transformed)) {
		error_init_reuse (error);

		mono_interp_transform_method (new_method, context, error);
		if (G_UNLIKELY (!is_ok (error)))
			THROW_EX (mono_error_convert_to_exception (error), ip);

		EXCEPTION_CHECKPOINT;
	}

	// It's possible for the caller stack frame to be smaller than the callee stack frame
	// (at the interp level).
	context->stack_pointer = (guchar *) frame->stack + new_method->alloca_size;
	frame->imethod = new_method;
	frame_root_code_owner (frame);
	ip = frame->imethod->code;

	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
