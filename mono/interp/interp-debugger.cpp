/**
 * \file
 * \brief What the debugger and a stack trace ask an interpreted frame for.
 */

#include "config.h"

#include "interp-callbacks.hpp"
#include "interp-internals.h"
#include "interp-internals.hpp"
#include "interp-call.hpp"
#include "interp-imethod.hpp"
#include "mintops.h"

#include <mono/metadata/debug-internals.h>
#include <mono/metadata/mono-debug.h>
#include <mono/mini/mini-runtime.h>

using mono::interp::get_arg_offset;
using mono::interp::get_arg_offset_fast;
using mono::interp::stackval_from_data;
using mono::interp::stackval_to_data;

using mono::interp::get_arg_offset;
using mono::interp::get_arg_offset_fast;
using mono::interp::stackval_from_data;
using mono::interp::stackval_to_data;

void
interp_frame_arg_to_data (MonoInterpFrameHandle frame, MonoMethodSignature *sig, int index, gpointer data)
{
	InterpFrame *iframe = (InterpFrame*)frame;
	InterpMethod *imethod = iframe->imethod;

	// If index == -1, we finished executing an InterpFrame and the result is at the bottom of the stack.
	if (index == -1)
		stackval_to_data (sig->ret, iframe->stack, data, TRUE);
	else if (sig->hasthis && index == 0)
		*(gpointer*)data = iframe->stack->data.p;
	else
		stackval_to_data (sig->params [index - sig->hasthis], STACK_ADD_BYTES (iframe->stack, get_arg_offset (imethod, sig, index)), data, sig->pinvoke);
}

void
interp_data_to_frame_arg (MonoInterpFrameHandle frame, MonoMethodSignature *sig, int index, gconstpointer data)
{
	InterpFrame *iframe = (InterpFrame*)frame;
	InterpMethod *imethod = iframe->imethod;

	// Get result from pinvoke call, put it directly on top of execution stack in the caller frame
	if (index == -1)
		stackval_from_data (sig->ret, iframe->stack, data, TRUE);
	else if (sig->hasthis && index == 0)
		iframe->stack->data.p = *(gpointer*)data;
	else
		stackval_from_data (sig->params [index - sig->hasthis], STACK_ADD_BYTES (iframe->stack, get_arg_offset (imethod, sig, index)), data, sig->pinvoke);
}

gpointer
interp_frame_arg_to_storage (MonoInterpFrameHandle frame, MonoMethodSignature *sig, int index)
{
	InterpFrame *iframe = (InterpFrame*)frame;
	InterpMethod *imethod = iframe->imethod;

	if (index == -1)
		return iframe->stack;
	else
		return STACK_ADD_BYTES (iframe->stack, get_arg_offset (imethod, sig, index));
}

static const guint8*
decode_uleb128 (const guint8 *p, guint32 *out)
{
	guint32 value = 0;
	int shift = 0;

	while (TRUE) {
		guint8 b = *p++;

		value |= (guint32) (b & 0x7f) << shift;
		if (!(b & 0x80))
			break;
		shift += 7;
	}

	*out = value;
	return p;
}

/*
 * imethod_il_offset:
 *
 *   Return the IL offset in effect at an offset into a method's bytecode, or -1
 * if it is not known. A compiled body answers the same question from its own
 * per-body map, so a stack trace reads the same either side of a promotion.
 */
static int
imethod_il_offset (InterpMethod *imethod, int native_offset)
{
	if (!imethod || !imethod->line_numbers || native_offset < 0)
		return -1;

	const guint8 *p = imethod->line_numbers;
	const guint8 *end = p + imethod->line_numbers_size;
	guint32 native = 0;
	gint32 il = 0;
	int result = -1;

	/*
	 * The entries ascend by bytecode offset, so the one in effect is the last
	 * starting at or before the offset asked about. Walking them is fine: this
	 * runs only while a stack trace is being built.
	 */
	while (p < end) {
		guint32 native_delta, il_delta;

		p = decode_uleb128 (p, &native_delta);
		p = decode_uleb128 (p, &il_delta);

		native += native_delta;
		il += (gint32) (il_delta >> 1) ^ -(gint32) (il_delta & 1);

		if ((int) native > native_offset)
			break;

		result = il;
	}

	return result;
}

/*
 * interp_il_offset_from_native_offset:
 *
 *   Return the IL offset in effect at an offset into a method's bytecode, for a
 * method named rather than held. Finding it means the per-domain table, so this
 * is for callers that can take a lock.
 */
int
interp_il_offset_from_native_offset (MonoDomain *domain, MonoMethod *method, int native_offset)
{
	return imethod_il_offset (lookup_imethod (domain, method), native_offset);
}

/*
 * interp_frame_il_offset:
 *
 *   Return the IL offset in effect in a frame. A frame holds its own method, so
 * this reaches the map without a lock and a signal handler can ask.
 */
int
interp_frame_il_offset (MonoInterpFrameHandle frame, int native_offset)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	return imethod_il_offset (iframe ? iframe->imethod : NULL, native_offset);
}

MonoJitInfo*
interp_find_jit_info (MonoDomain *domain, MonoMethod *method)
{
	InterpMethod* imethod;

	imethod = lookup_imethod (domain, method);
	if (imethod)
		return imethod->jinfo;
	else
		return NULL;
}

void
interp_set_breakpoint (MonoJitInfo *jinfo, gpointer ip)
{
	guint16 *code = (guint16*)ip;
	g_assert (*code == MINT_SDB_SEQ_POINT);
	*code = MINT_SDB_BREAKPOINT;
}

void
interp_clear_breakpoint (MonoJitInfo *jinfo, gpointer ip)
{
	guint16 *code = (guint16*)ip;
	g_assert (*code == MINT_SDB_BREAKPOINT);
	*code = MINT_SDB_SEQ_POINT;
}

MonoJitInfo*
interp_frame_get_jit_info (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);
	return iframe->imethod->jinfo;
}

gpointer
interp_frame_get_arg (MonoInterpFrameHandle frame, int pos)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);

	return (char*)iframe->stack + get_arg_offset_fast (iframe->imethod, pos + iframe->imethod->hasthis);
}

gpointer
interp_frame_get_local (MonoInterpFrameHandle frame, int pos)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);

	return frame_locals (iframe) + iframe->imethod->local_offsets [pos];
}

gpointer
interp_frame_get_this (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	g_assert (iframe->imethod);
	g_assert (iframe->imethod->hasthis);
	return iframe->stack;
}

MonoInterpFrameHandle
interp_frame_get_parent (MonoInterpFrameHandle frame)
{
	InterpFrame *iframe = (InterpFrame*)frame;

	return iframe->parent;
}

void
interp_start_single_stepping (void)
{
	mono_interp_ss_enabled = TRUE;
}

void
interp_stop_single_stepping (void)
{
	mono_interp_ss_enabled = FALSE;
}
