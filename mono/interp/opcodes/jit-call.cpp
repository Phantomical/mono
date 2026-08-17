#include "config.h"

/**
 * \file
 * \brief The opcodes that hand a call to the compiled tier.
 */

#include "mintops.hpp"
#include "mono/interp/runtime/internals.hpp"
#include "mono/interp/runtime/jit-call.hpp"
#include "mono/interp/runtime/stackval.hpp"
#include "mono/interp/interp.hpp"
#include "mono/metadata/object-internals.h"
#include "mono/utils/mono-error-internals.h"

namespace mono::interp {

MONO_INTERP_OP_IMPL (MINT_JIT_CALL)
{
	auto rmethod = static_cast<InterpMethod *> (frame->imethod->data_items[ip[2]]);

	error_init_reuse (error);
	/* for calls, have ip pointing at the start of next instruction */
	frame->state.ip = ip + 3;
	do_jit_call (reinterpret_cast<stackval *> ((locals + ip[1])), frame, rmethod, error);
	if (!is_ok (error))
		THROW_EX (mono_error_convert_to_exception (error), ip);

	CHECK_RESUME_STATE (context);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_JIT_CALL2)
{
	g_error ("MINT_JIT_CALL2 shouldn't be used");
}

} // namespace mono::interp
