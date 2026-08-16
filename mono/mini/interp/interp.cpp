#include "mono/mini/interp/interp.hpp"
#include "frame-data.hpp"
#include "mono/metadata/handle.h"
#include "mono/mini/mini.h"
#include "mono/utils/mono-context.h"
#include <cstddef>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

namespace mono::interp {

const InterpState::OpFunc InterpState::optable[] = {
#define OPDEF(name, b, c, d, e, f) &InterpState::entry_##name,
#include "mintops.def"
#undef OPDEF
};

void
InterpState::exec_invalid_opcode (InterpState *state)
{
	std::string message;
	llvm::raw_string_ostream stream (message);
	stream << llvm::format ("invalid opcode: %04x at 0x%x", *state->ip,
	                        (unsigned) (state->ip - state->frame->imethod->code));
	llvm::reportFatalInternalError (message.c_str ());
}

void
InterpState::interp_throw (MonoException *ex, const guint16 *ip, bool rethrow)
{
	{
		ERROR_DECL (error);
		auto guard = LMFGuard (frame);

		// When explicitly throwing exceptions we pass the ip of the instruction that
		// throws the exception. This offsets the subtraction from interp_frame_get_ip, so
		// we don't end up in the previous instruction.
		frame->state.ip = ip + 1;

		if (mono_object_isinst_checked ((MonoObject *) ex, mono_defaults.exception_class, error)) {
			MonoException *mono_ex = ex;
			if (!rethrow) {
				mono_ex->stack_trace = nullptr;
				mono_ex->trace_ips = nullptr;
			}
		}

		mono_error_assert_ok (error);

		MonoContext ctx;
		std::memset (&ctx, 0, sizeof (MonoContext));
		MONO_CONTEXT_SET_SP (&ctx, frame);

		// Call the JIT EH code. The EH code will call back to us using
		// mono_interp_set_resume_state/run_finally/run_filter.
		// Since ctx.ip is 0, this will start unwinding from the LMF frame pushed above,
		// which points to our frames.
		mono_handle_exception (&ctx, (MonoObject *) ex);
		if (MONO_CONTEXT_GET_IP (&ctx) != 0) {
			/* We need to unwind into non-interpreter code */
			mono_restore_context (&ctx);
			g_assert_not_reached ();
		}
	}

	g_assert (context->has_resume_state);
}

MONO_INTERP_ENTRY (exec_resume, resume);

static void
clear_resume_state (ThreadContext *context)
{
	context->has_resume_state = 0;
	context->handler_frame = NULL;
	context->handler_ei = NULL;
	g_assert (context->exc_gchandle);
	mono_gchandle_free_internal (context->exc_gchandle);
	context->exc_gchandle = 0;
}

InterpState::OpFunc
MONO_ALWAYS_INLINE InterpState::resume ()
{
	g_assert (context->has_resume_state);
	g_assert (frame->imethod);

	if (frame == context->handler_frame) {
		/*
		 * When running finally blocks, we can have the same frame twice on the stack. If we have
		 * clause_args information, we need to check whether resuming should happen inside this
		 * finally block, or in some other part of the method, in which case we need to exit.
		 */
		if (clause_args && frame == clause_args->exec_frame
		    && context->handler_ip >= clause_args->end_at_ip)
			return &exec_exit;

		/* Set the current execution state to the resume state in context */
		ip = context->handler_ip;
		/* spec says stack should be empty at endfinally so it should be at the start too */
		locals = (guchar *) frame->stack;
		g_assert (context->exc_gchandle);
		// Write the exception on to the first slot on the excecution stack
		LOCAL_VAR (frame->imethod->total_locals_size, MonoObject *) =
			mono_gchandle_get_target_internal (context->exc_gchandle);

		clear_resume_state (context);
		/* The resume site cleared the marker if it skipped frames, so put it back. */
		context->current_frame = frame;

		MONO_INTERP_DISPATCH ();
	} else if (clause_args && frame == clause_args->exec_frame) {
		/*
		 * This frame doesn't handle the resume state and it is the first frame invoked from EH.
		 * We can't just return to parent. We must first exit the EH mechanism and start resuming
		 * again from the original frame.
		 */
		return &exec_exit;
	}

	// Because we are resuming in another frame, bypassing a normal ret opcode,
	// we need to make sure to reset the localloc stack
	frame_data_allocator_pop (&context->data_stack, frame);

	return &exec_exit_frame;
}

MONO_INTERP_ENTRY(exec_exit, exit);

static void real_exit(InterpState* state) {}

InterpState::OpFunc
MONO_ALWAYS_INLINE InterpState::exit()
{
	// Make sure the return value stays below the stack pointer
	if (!clause_args)
		context->stack_pointer = (guchar*)frame->stack + frame->imethod->alloca_size;

	/* Our frames go away with this invocation, so hand the marker back to the one below. */
	context->current_frame = outer_current_frame;

	return &real_exit;
}


} // namespace mono::interp
