#ifndef __MONO_INTERP_INTERP_METHOD_HPP__
#define __MONO_INTERP_INTERP_METHOD_HPP__

/**
 * \file
 * \brief Getting a method ready to run, and counting its way to tier 1.
 */

#include "internals.hpp"
#include "lmf.hpp"
#include "trace.hpp"

#include <mono/llvm/runtime.h>
#include <mono/metadata/appdomain.h>
#include <mono/mini/mini.h>
#include <mono/utils/atomic.h>

namespace mono::interp {

// Initialize the tiering counter, if it hasn't already been initialized.
inline void
arm_tier_counter (gpointer imethod_ptr, gint32 calls)
{
	InterpMethod *imethod = (InterpMethod *) imethod_ptr;

	mono_atomic_cas_i32 (&imethod->tier_counter, calls > 0 ? calls : -1, 0);
}

// Check whether we should start a background compilation of this method to tier1.
inline MONO_NEVER_INLINE void
interp_check_call_promotion (InterpMethod *imethod)
{
	gint32 left;

	left = mono_atomic_dec_i32 (&imethod->tier_counter);
	if (left != 0)
		return;

	/*
	 * A refused request is the counter spent for nothing, and nothing else
	 * arms it again: arm_tier_counter () is reached once per method,
	 * from whichever of resolve_code_type () and the backend's entry sees it
	 * first. Arming it here is what makes the loss cost this method another
	 * threshold of calls rather than the rest of the process.
	 */
	if (!mono_llvm_jit_request_promotion (imethod->method, imethod->domain))
		arm_tier_counter (imethod, mono_llvm_jit_tier0_calls (imethod->method));
}

/*
 * Transforms the method frame is about to run, and returns what that threw or null.
 *
 * A frame whose imethod is not transformed yet is incomplete, so the transform runs
 * under the parent instead. A root frame has no parent and no walk to satisfy.
 */
inline MonoException *
do_transform_method (InterpFrame *frame, ThreadContext *context)
{
	MonoLMFExt ext;
	gboolean push_lmf = frame->parent != NULL;
	ERROR_DECL (error);

	if (push_lmf)
		interp_push_lmf (&ext, frame->parent);

	mono_interp_transform_method (frame->imethod, context, error);

	if (push_lmf)
		interp_pop_lmf (&ext);

	return mono_error_convert_to_exception (error);
}

/*
 * Makes frame ready to run, and returns whether it took the slow path. out_ex holds
 * what the transform threw, and is null when nothing did.
 *
 * A caller that took the slow path has to check out_ex and run an interruption
 * checkpoint. Both are rare, which is what keeps them out of the fast path.
 */
inline MONO_ALWAYS_INLINE gboolean
method_entry (ThreadContext *context, InterpFrame *frame, MonoException **out_ex)
{
	gboolean slow = FALSE;

	MONO_INTERP_TRACE_ENTER (context, frame);

	*out_ex = NULL;
	if (!G_UNLIKELY (frame->imethod->transformed)) {
		slow = TRUE;
		MonoException *ex = do_transform_method (frame, context);
		if (ex) {
			*out_ex = ex;
			/*
			 * Initialize the stack base pointer here, in the uncommon branch, so we don't
			 * need to check for it everytime when exitting a frame.
			 */
			frame->stack = (stackval *) context->stack_pointer;
			return slow;
		}
	}

	return slow;
}

/// Runs a class initializer, and returns what it threw or null.
inline MONO_NEVER_INLINE MonoException *
init_vtable (MonoVTable *vtable)
{
	ERROR_DECL (error);

	mono_runtime_class_init_full (vtable, error);
	if (!is_ok (error))
		return mono_error_convert_to_exception (error);
	return nullptr;
}
// The class init runs here, not at transform time, so that cctors run in
// program order.
#define INIT_VTABLE(vtable)                        \
	do {                                           \
		MonoVTable *__vtable = (vtable);           \
		if (G_UNLIKELY (!__vtable->initialized)) { \
			if (auto ex = init_vtable (__vtable))  \
				THROW_EX (ex, ip);                 \
		}                                          \
	} while (0)

} // namespace mono::interp

#endif
