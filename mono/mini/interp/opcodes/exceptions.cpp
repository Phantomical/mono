#include "mintops.h"
#include "mono/metadata/exception.h"
#include "mono/metadata/object-internals.h"
#include "mono/mini/interp/interp-internals.h"
#include "mono/mini/interp/interp.hpp"
#include "mono/utils/mono-threads.h"

namespace mono::interp {

MONO_INTERP_OP_IMPL (MINT_THROW)
{
	auto ex = LOCAL_VAR (ip[1], MonoException *);
	if (!ex)
		ex = mono_get_exception_null_reference ();

	THROW_EX (ex, ip);
}

// A rethrow keeps the stack trace the exception was caught with.
MONO_INTERP_OP_IMPL (MINT_RETHROW)
{
	THROW_EX_GENERAL (*(MonoException **) (frame_locals (frame) + ip[1]), ip, TRUE);
}

/*
 * Takes an exception from the stack and rethrows it. A wrapper uses this instead of
 * CEE_THROW so that it does not lose the stack trace.
 */
MONO_INTERP_OP_IMPL (MINT_MONO_RETHROW)
{
	auto exc = LOCAL_VAR (ip[1], MonoException *);
	if (!exc)
		exc = mono_get_exception_null_reference ();

	THROW_EX_GENERAL (exc, ip, TRUE);
}

/*
 * leave branches out of a protected block, which means running the finally bodies
 * between here and the target first. The transform put a MINT_CALL_HANDLER for each
 * of them ahead of this instruction, so by the time control arrives all that is left
 * is the branch.
 *
 * The _CHECK forms are the ones a thread abort can interrupt. They ask before the
 * branch, since a pending abort has to be raised while the handlers are still in
 * scope; the plain forms ask afterwards.
 */
#define IMPL_LEAVE(opcode, check, short_offset)                                       \
	MONO_INTERP_OP_IMPL (opcode)                                                      \
	{                                                                                 \
		if (check                                                                     \
		    && frame->imethod->method->wrapper_type != MONO_WRAPPER_RUNTIME_INVOKE) { \
			if (MonoException *abort_exc = mono_interp_leave (frame))                 \
				THROW_EX (abort_exc, ip);                                             \
		}                                                                             \
                                                                                      \
		ip += short_offset ? (gint16) *(ip + 1) : (gint32) READ32 (ip + 1);            \
                                                                                      \
		if (!check)                                                                   \
			EXCEPTION_CHECKPOINT;                                                     \
                                                                                      \
		MONO_INTERP_DISPATCH ();                                                      \
	}

IMPL_LEAVE (MINT_LEAVE, false, false);
IMPL_LEAVE (MINT_LEAVE_S, false, true);
IMPL_LEAVE (MINT_LEAVE_CHECK, true, false);
IMPL_LEAVE (MINT_LEAVE_S_CHECK, true, true);

/*
 * Calls a finally body without leaving this frame. The address to come back to is
 * kept in the clause's own slot, which MINT_ENDFINALLY reads.
 */
#define IMPL_CALL_HANDLER(opcode, short_offset)                                            \
	MONO_INTERP_OP_IMPL (opcode)                                                           \
	{                                                                                      \
		const guint16 *ret_ip = short_offset ? (ip + 3) : (ip + 4);                        \
		guint16 clause_index = *(ret_ip - 1);                                              \
                                                                                           \
		*(const guint16 **) (locals + frame->imethod->clause_data_offsets[clause_index]) = \
			ret_ip;                                                                        \
                                                                                           \
		ip += short_offset ? (gint16) *(ip + 1) : (gint32) READ32 (ip + 1);                \
                                                                                           \
		MONO_INTERP_DISPATCH ();                                                           \
	}

IMPL_CALL_HANDLER (MINT_CALL_HANDLER, false);
IMPL_CALL_HANDLER (MINT_CALL_HANDLER_S, true);

MONO_INTERP_OP_IMPL (MINT_ENDFINALLY)
{
	mono_threads_end_abort_protected_block ();

	guint16 clause_index = ip[1];
	auto ret_ip = *(guint16 **) (locals + frame->imethod->clause_data_offsets[clause_index]);

	if (!ret_ip) {
		// this clause was called from EH, return to eh
		g_assert (clause_args && clause_args->exec_frame == frame);
		return &exec_exit;
	}

	ip = ret_ip;

	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_ENDFILTER)
{
	/* top of stack is result of filter */
	frame->retval->data.i = LOCAL_VAR (ip[1], gint32);

	return &exec_exit;
}

MONO_INTERP_OP_IMPL (MINT_START_ABORT_PROT)
{
	mono_threads_begin_abort_protected_block ();

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
