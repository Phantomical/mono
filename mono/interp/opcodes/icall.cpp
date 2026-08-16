#include "config.h"

/**
 * \file
 * \brief Calling what the runtime implements in C rather than in IL.
 */

#include "mintops.h"
#include "mono/interp/interp-frame.hpp"
#include "mono/interp/interp-icall.hpp"
#include "mono/interp/interp-internals.h"
#include "mono/interp/interp-stackval.hpp"
#include "mono/interp/interp.hpp"
#include "mono/metadata/object-internals.h"
#include "mono/utils/mono-error-internals.h"

namespace mono::interp {

MONO_INTERP_OP_IMPL (MINT_CALLRUN)
{
#ifndef ENABLE_NETCORE
	auto target_method = (MonoMethod *) frame->imethod->data_items[ip[2]];
	auto sig = (MonoMethodSignature *) frame->imethod->data_items[ip[3]];

	if (MonoException *ex = ves_imethod (frame, target_method, sig, (stackval *) (locals + ip[1])))
		THROW_EX (ex, ip);
#else
	g_assert_not_reached ();
#endif

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

/*
 * The variable arguments were pushed by the caller, so their layout is described by
 * the signature at its call site rather than by this method's own. The caller is
 * suspended at the instruction after the call, which is what the walk back to
 * MINT_CALL_VARARG counts from.
 */
MONO_INTERP_OP_IMPL (MINT_INIT_ARGLIST)
{
	const guint16 *call_ip = frame->parent->state.ip - 5;
	g_assert_checked (*call_ip == MINT_CALL_VARARG);

	int params_stack_size = call_ip[4];
	auto sig = (MonoMethodSignature *) frame->parent->imethod->data_items[call_ip[3]];

	// we are being overly conservative with the size here, for simplicity
	gpointer arglist = frame_data_allocator_alloc (&context->data_stack, frame,
	                                               params_stack_size + MINT_STACK_SLOT_SIZE);

	init_arglist (frame, sig, STACK_ADD_BYTES (frame->stack, ip[2]), (char *) arglist);

	// save the arglist for future access with MINT_ARGLIST
	LOCAL_VAR (ip[1], gpointer) = arglist;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

/*
 * An icall with no wrapper. The opcode names the arity and whether a value comes
 * back, which is what picks the prototype the target is called through.
 */
#define IMPL_ICALL(opcode)                                                       \
	MONO_INTERP_OP_IMPL (opcode)                                                 \
	{                                                                            \
		/* for calls, have ip pointing at the start of next instruction */       \
		frame->state.ip = ip + 3;                                                \
		do_icall_wrapper (frame, nullptr, opcode, (stackval *) (locals + ip[1]), \
		                  frame->imethod->data_items[ip[2]], FALSE);             \
		EXCEPTION_CHECKPOINT_GC_UNSAFE;                                          \
		CHECK_RESUME_STATE (context);                                            \
                                                                                 \
		MONO_INTERP_OP_ADVANCE ();                                               \
		MONO_INTERP_DISPATCH ();                                                 \
	}

IMPL_ICALL (MINT_ICALL_V_V);
IMPL_ICALL (MINT_ICALL_V_P);
IMPL_ICALL (MINT_ICALL_P_V);
IMPL_ICALL (MINT_ICALL_P_P);
IMPL_ICALL (MINT_ICALL_PP_V);
IMPL_ICALL (MINT_ICALL_PP_P);
IMPL_ICALL (MINT_ICALL_PPP_V);
IMPL_ICALL (MINT_ICALL_PPP_P);
IMPL_ICALL (MINT_ICALL_PPPP_V);
IMPL_ICALL (MINT_ICALL_PPPP_P);
IMPL_ICALL (MINT_ICALL_PPPPP_V);
IMPL_ICALL (MINT_ICALL_PPPPP_P);
IMPL_ICALL (MINT_ICALL_PPPPPP_V);
IMPL_ICALL (MINT_ICALL_PPPPPP_P);

MONO_INTERP_OP_IMPL (MINT_MONO_RETOBJ)
{
	MonoMethodSignature *sig = mono_method_signature_internal (frame->imethod->method);

	stackval_from_data (sig->ret, frame->stack, LOCAL_VAR (ip[1], gpointer), sig->pinvoke);
	frame_data_allocator_pop (&context->data_stack, frame);

	return &exec_exit_frame;
}

} // namespace mono::interp
