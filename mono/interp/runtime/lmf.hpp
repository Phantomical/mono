#ifndef __MONO_INTERP_INTERP_LMF_HPP__
#define __MONO_INTERP_INTERP_LMF_HPP__

/**
 * \file
 * \brief Publishing an interpreter frame to the runtime's LMF chain.
 */

#include "internals.hpp"

#include <mono/mini/mini-runtime.h>
#include <mono/utils/mono-context.h>

#if defined(MONO_CROSS_COMPILE) || defined(HOST_WASM)
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) (ext).kind = MONO_LMFEXT_INTERP_EXIT;

#elif defined(MONO_ARCH_HAS_NO_PROPER_MONOCTX)
/* some platforms, e.g. appleTV, don't provide us a precise MonoContext
 * (registers are not accurate), thus resuming to the label does not work. */
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) (ext).kind = MONO_LMFEXT_INTERP_EXIT;
#elif defined(_MSC_VER)
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) \
	(ext).kind = MONO_LMFEXT_INTERP_EXIT_WITH_CTX;     \
	(ext).interp_exit_label_set = FALSE;               \
	MONO_CONTEXT_GET_CURRENT ((ext).ctx);              \
	if ((ext).interp_exit_label_set == FALSE)          \
		mono_arch_do_ip_adjustment (&(ext).ctx);       \
	if ((ext).interp_exit_label_set == TRUE)           \
		goto exit_label;                               \
	(ext).interp_exit_label_set = TRUE;
#elif defined(MONO_ARCH_HAS_MONO_CONTEXT)
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) \
	(ext).kind = MONO_LMFEXT_INTERP_EXIT_WITH_CTX;     \
	MONO_CONTEXT_GET_CURRENT ((ext).ctx);              \
	MONO_CONTEXT_SET_IP (&(ext).ctx, (&&exit_label));  \
	mono_arch_do_ip_adjustment (&(ext).ctx);
#else
#define INTERP_PUSH_LMF_WITH_CTX_BODY(ext, exit_label) g_error ("requires working mono-context");
#endif

/* INTERP_PUSH_LMF_WITH_CTX:
 *
 * same as interp_push_lmf, but retrieving and attaching MonoContext to it.
 * This is needed to resume into the interp when the exception is thrown from
 * native code (see ./mono/tests/install_eh_callback.exe).
 *
 * This must be a macro in order to retrieve the right register values for
 * MonoContext.
 */
#define INTERP_PUSH_LMF_WITH_CTX(frame, ext, exit_label) \
	memset (&(ext), 0, sizeof (MonoLMFExt));             \
	(ext).interp_exit_data = (frame);                    \
	INTERP_PUSH_LMF_WITH_CTX_BODY ((ext), exit_label);   \
	mono_push_lmf (&(ext));

namespace mono::interp {

/*
 * interp_push_lmf:
 *
 * Push an LMF frame on the LMF stack
 * to mark the transition to native code.
 * This is needed for the native code to
 * be able to do stack walks.
 */
inline void
interp_push_lmf (MonoLMFExt *ext, InterpFrame *frame)
{
	/*
	 * Only these two fields and lmf.previous_lmf, which mono_push_lmf ()
	 * writes, are ever read back: the rest of the MonoLMF is documented as
	 * invalid once its second lowest bit marks the entry as an ext, and ctx
	 * belongs to the WITH_CTX kind. Zeroing the whole thing instead would
	 * clear a MonoContext, which is most of the ~450 bytes here and costs
	 * around a fifth of a jit call.
	 */
	ext->kind = MONO_LMFEXT_INTERP_EXIT;
	ext->interp_exit_data = frame;

	mono_push_lmf (ext);
}

inline void
interp_pop_lmf (MonoLMFExt *ext)
{
	mono_pop_lmf (&ext->lmf);
}

} // namespace mono::interp

#endif
