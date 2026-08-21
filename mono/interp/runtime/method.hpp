#ifndef __MONO_INTERP_INTERP_METHOD_HPP__
#define __MONO_INTERP_INTERP_METHOD_HPP__

/**
 * \file
 * \brief Getting a method ready to run, and counting its way to tier 1.
 */

#include "internals.hpp"
#include "lmf.hpp"
#include "trace.hpp"

#include <mono/metadata/appdomain.h>
#include <mono/mini/mini.h>
#include <mono/utils/atomic.h>

namespace mono::interp {

/// Counts one call against imethod's way to tier 1, and asks for the method
/// once the count has run out.
void interp_check_call_promotion (InterpMethod *imethod);

/// Transforms the method that frame is about to run, and returns what that
/// threw or null.
///
/// A frame whose imethod is not transformed yet is incomplete, so the
/// transform runs under the parent instead. A root frame has no parent and no
/// walk to satisfy.
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

/// Makes frame ready to run, and returns whether it took the slow path. out_ex
/// holds what the transform threw, and is null when nothing did.
///
/// A caller that took the slow path has to check out_ex and run an
/// interruption checkpoint. Both are rare, which is what keeps them out of the
/// fast path.
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
			// Initialize the stack base pointer here, in the uncommon branch, so we
			// don't need to check for it every time a frame exits.
			frame->stack = reinterpret_cast<stackval *> (context->stack_pointer);
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
