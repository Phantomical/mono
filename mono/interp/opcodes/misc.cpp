#include "mintops.h"
#include "mono/metadata/appdomain.h"
#include "mono/metadata/exception.h"
#include "mono/interp/frame-data.hpp"
#include "mono/interp/interp.hpp"
#include "mono/utils/atomic.h"
#include "mono/utils/mono-threads.h"
#include "mono/utils/mono-tls-inline.h"
#include <cstring>

namespace mono::interp {

MONO_INTERP_OP_IMPL (MINT_INITLOCALS)
{
	std::memset (locals + ip[1], 0, ip[2]);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_CPBLK)
{
	auto dst = LOCAL_VAR (ip[1], gpointer);
	auto src = LOCAL_VAR (ip[2], gpointer);
	if (G_UNLIKELY (!dst || !src))
		THROW_EX (mono_get_exception_null_reference (), ip);

	// FIXME: value and size may be int64
	std::memcpy (dst, src, LOCAL_VAR (ip[3], gint32));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INITBLK)
{
	gpointer dest = LOCAL_VAR (ip[1], gpointer);
	NULL_CHECK (dest);

	// FIXME: value and size may be int64
	std::memset (dest, LOCAL_VAR (ip[2], gint32), LOCAL_VAR (ip[3], gint32));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INITOBJ)
{
	gpointer destination = LOCAL_VAR (ip[1], gpointer);

	NULL_CHECK (destination);
	std::memset (destination, 0, ip[2]);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_CKNULL)
{
	auto o = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (o);
	LOCAL_VAR (ip[1], MonoObject *) = o;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LOCALLOC)
{
	int len = LOCAL_VAR (ip[2], gint32);
	gpointer mem = frame_data_allocator_alloc (&context->data_stack, frame,
	                                           ALIGN_TO (len, MINT_VT_ALIGNMENT));

	if (frame->imethod->init_locals)
		std::memset (mem, 0, len);
	LOCAL_VAR (ip[1], gpointer) = mem;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

/*
 * Release what this frame has localloc'd without leaving it, which a tail call to
 * the method itself needs: the frame stays, but the invocation that owned the
 * memory does not.
 */
MONO_INTERP_OP_IMPL (MINT_LOCALLOC_UNWIND)
{
	frame_data_allocator_pop (&context->data_stack, frame);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_CHECKPOINT)
{
	/*
	 * The runtime stops this thread here and walks its stack, so the frame has to
	 * say which instruction it is on. The +1 offsets the subtraction
	 * interp_frame_get_ip does for call sites.
	 */
	frame->state.ip = ip + 1;
	EXCEPTION_CHECKPOINT;
	frame->state.ip = nullptr;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_SAFEPOINT)
{
	/* Publish the ip for the poll, as MINT_CHECKPOINT does. */
	frame->state.ip = ip + 1;
	EXCEPTION_CHECKPOINT;

	if (G_UNLIKELY (mono_polling_required)) {
		g_assert (!context->has_resume_state);
		context->safepoint_frame = frame;
		mono_threads_safepoint ();
		context->safepoint_frame = nullptr;
	}

	frame->state.ip = nullptr;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDTOKEN)
{
	// FIXME same as MINT_MONO_LDPTR
	LOCAL_VAR (ip[1], gpointer) = frame->imethod->data_items[ip[2]];

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_MONO_LDPTR)
{
	LOCAL_VAR (ip[1], gpointer) = frame->imethod->data_items[ip[2]];

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_MONO_LDDOMAIN)
{
	LOCAL_VAR (ip[1], gpointer) = mono_domain_get ();

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_MONO_SGEN_THREAD_INFO)
{
	LOCAL_VAR (ip[1], gpointer) = mono_tls_get_sgen_thread_info ();

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

// The frame stands in for the stack pointer a compiled method would have had.
MONO_INTERP_OP_IMPL (MINT_MONO_GET_SP)
{
	LOCAL_VAR (ip[1], gpointer) = frame;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_MONO_MEMORY_BARRIER)
{
	mono_memory_barrier ();

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_MONO_ATOMIC_STORE_I4)
{
	mono_atomic_store_i32 (LOCAL_VAR (ip[1], gint32 *), LOCAL_VAR (ip[2], gint32));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_MONO_EXCHANGE_I8)
{
	gint64 *dest = LOCAL_VAR (ip[2], gint64 *);
	gint64 exch = LOCAL_VAR (ip[3], gint64);

#if SIZEOF_VOID_P == 4
	// A 32-bit target has no atomic that reaches a misaligned 64-bit slot.
	if (G_UNLIKELY (((size_t) dest) & 0x7)) {
		mono_interlocked_lock ();
		gint64 result = *dest;
		*dest = exch;
		mono_interlocked_unlock ();

		LOCAL_VAR (ip[1], gint64) = result;

		MONO_INTERP_OP_ADVANCE ();
		MONO_INTERP_DISPATCH ();
	}
#endif

	LOCAL_VAR (ip[1], gint64) = mono_atomic_xchg_i64 (dest, exch);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
