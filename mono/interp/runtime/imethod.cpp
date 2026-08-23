/**
 * \file
 * \brief What the interpreter keeps for a method, how it is invalidated, and
 * how the method counts its way to tier 1.
 */

#include "config.h"

#include "callbacks.hpp"
#include "internals.hpp"
#include "method.hpp"
#include "trace.hpp"
#include "entry.hpp"
#include "imethod.hpp"
#include "sharing.hpp"

#include <mono/metadata/marshal.h>
#include <mono/metadata/metadata-update.h>
#include <mono/mini/domain-method.h>
#include <mono/mini/domain-method.hpp>
#include <mono/mini/mini-runtime.h>
#include <mono/utils/mono-logger-internals.h>
#include <mono/utils/mono-threads.h>

namespace mono::interp {

namespace {

/// Sets how many calls imethod takes before it is asked for as tier 1.
///
/// A count of zero or less means the method never promotes.
void
arm_tier_counter (InterpMethod *imethod, gint32 calls)
{
	// The call sites only reach interp_check_call_promotion () while tier_counter
	// tests positive. That test is what makes a count of zero or less permanent.
	mono_atomic_store_i32 (&imethod->tier_counter, calls > 0 ? calls : -1);
}

} // namespace

InterpMethod *
lookup_imethod (MonoDomain *domain, MonoMethod *method)
{
	MonoDomainMethod *dm = domain_method_find (domain, method);

	return dm != nullptr ? dm->interp_method () : nullptr;
}

gpointer
interp_get_remoting_invoke (MonoMethod *method, gpointer addr, MonoError *error)
{
#ifndef DISABLE_REMOTING
	InterpMethod *imethod;

	if (addr) {
		imethod = lookup_method_pointer (mono_domain_get (), addr);
	} else {
		g_assert (method);
		imethod = mono_interp_get_imethod (mono_domain_get (), method, error);
		return_val_if_nok (error, NULL);
	}
	g_assert (imethod);
	g_assert (mono_use_interpreter);

	MonoMethod *remoting_invoke_method = mono_marshal_get_remoting_invoke (imethod->method, error);
	return_val_if_nok (error, NULL);
	return mono_interp_get_imethod (mono_domain_get (), remoting_invoke_method, error);
#else
	g_assert_not_reached ();
	return NULL;
#endif
}

/// Transforms method now rather than on the first call into it. That way,
/// whether the interpreter can run it at all is known before anything can.
///
/// Returns FALSE with error set when it cannot.
///
/// This is what create_method_pointer () does for itself when asked to
/// compile. It is separate so that a caller which has to publish the entry
/// first can still find out. Transforming runs the class initializer, so a
/// caller holding a lock that managed code can reach has to get the entry
/// reachable before it asks.
gboolean
interp_transform_method (MonoMethod *method, MonoError *error)
{
	InterpMethod *imethod = mono_interp_get_imethod (mono_domain_get (), method, error);

	if (!is_ok (error))
		return FALSE;

	if (imethod->transformed)
		return TRUE;

	mono_interp_transform_method (imethod, mono_interp_get_context (), error);
	return is_ok (error);
}

/// Counts one call against imethod's way to tier 1, and asks for the method
/// once the count has run out.
///
/// This tests for a count that has run out, not for the call that ended it.
/// Several threads can decrement at once, so the one landing on zero is not
/// necessarily the last to arrive.
void
interp_check_call_promotion (InterpMethod *imethod)
{
	if (mono_atomic_dec_i32 (&imethod->tier_counter) > 0)
		return;

	if (mono_promote_method (imethod->method, imethod->domain))
		return;

	/*
	 * A refused promotion spends the count for nothing, so this re-arms it. The
	 * loss then costs this method another threshold of calls, not the rest of
	 * the process.
	 */
	if (MonoDomainMethod *dm = domain_method_find (imethod->domain, imethod->method))
		arm_tier_counter (imethod, dm->tier_calls.load (std::memory_order_relaxed));
}

/// method now has native code in domain, so calls to it from interpreted code
/// go there instead of interpreting it.
///
/// This does not create the interpreter's record for method. One it has never
/// seen settles this for itself the first time it is called. It can only be
/// settled the other way while there is no code.
void
interp_method_compiled (MonoDomain *domain, MonoMethod *method)
{
	InterpMethod *imethod = lookup_imethod (domain, method);

	if (imethod == NULL || imethod->code_type == IMETHOD_CODE_COMPILED)
		return;

	if (mono_interp_jit_call_marshallable (method, mono_method_signature_internal (method)))
		imethod->code_type = IMETHOD_CODE_COMPILED;
}

static void
copy_imethod_for_frame (MonoDomain *domain, InterpFrame *frame)
{
	InterpMethod *copy =
		static_cast<InterpMethod *> (mono_domain_alloc0 (domain, sizeof (InterpMethod)));
	memcpy (copy, frame->imethod, sizeof (InterpMethod));
	frame->imethod = copy;
	/* The copy stays alive until the domain unloads: mono_domain_alloc0 () has
	 * no free. It is never reclaimed, even after the InterpFrame that made it
	 * pops.
	 */
}

void
interp_metadata_update_init (MonoError *error)
{
	if ((mono_interp_opt & INTERP_OPT_INLINE) != 0)
		mono_error_set_execution_engine (
			error, "Interpreter inlining must be turned off for metadata updates");
}

static void
metadata_update_backup_frames (MonoDomain *domain, MonoThreadInfo *info, InterpFrame *frame)
{
	while (frame) {
		mono_trace (G_LOG_LEVEL_DEBUG, MONO_TRACE_METADATA_UPDATE,
		            "threadinfo=%p, copy imethod for method=%s", info,
		            mono_method_full_name (frame->imethod->method, 1));
		copy_imethod_for_frame (domain, frame);
		frame = frame->parent;
	}
}

static void
metadata_update_prepare_to_invalidate (MonoDomain *domain)
{
	/* (1) make a copy of imethod for every interpframe that is on the stack,
	 * so we do not invalidate currently running methods */

	FOREACH_THREAD_EXCLUDE (info, MONO_THREAD_INFO_FLAGS_NO_GC)
	{
		if (!info || !info->jit_data)
			continue;

		ThreadContext *context = static_cast<ThreadContext *> (info->jit_data->interp_context);

		/* If the thread was in the interpreter and hit a safepoint
		 * opcode and suspended, backup the frames since the last lmf.
		 */
		if (context && context->safepoint_frame) {
			mono_trace (G_LOG_LEVEL_DEBUG, MONO_TRACE_METADATA_UPDATE,
			            "threadinfo=%p, has safepoint frame %p", info, context->safepoint_frame);
			metadata_update_backup_frames (domain, info, context->safepoint_frame);
		}

		MonoLMF *lmf = info->jit_data->lmf;
		while (lmf) {
			if (((gsize) lmf->previous_lmf) & 2) {
				MonoLMFExt *ext = reinterpret_cast<MonoLMFExt *> (lmf);
				if (ext->kind == MONO_LMFEXT_INTERP_EXIT
				    || ext->kind == MONO_LMFEXT_INTERP_EXIT_WITH_CTX) {
					InterpFrame *frame = static_cast<InterpFrame *> (ext->interp_exit_data);
					metadata_update_backup_frames (domain, info, frame);
				}
			}
			lmf = (MonoLMF *) (((gsize) lmf->previous_lmf) & ~3);
		}
	}
	FOREACH_THREAD_END

	/* (2) invalidate all the registered imethods */
}

void
interp_invalidate_transformed (MonoDomain *domain)
{
	gboolean need_stw_restart = FALSE;
#ifdef ENABLE_METADATA_UPDATE
	need_stw_restart = TRUE;
	mono_gc_stop_world ();
	metadata_update_prepare_to_invalidate (domain);
#endif
	domain_method_foreach (domain, [] (MonoDomainMethod &dm) {
		if (InterpMethod *imethod = dm.interp_method ())
			imethod->transformed = FALSE;
	});

	if (need_stw_restart)
		mono_gc_restart_world ();
}

} // namespace mono::interp

/* Outside the namespace, because internals.hpp declares them there. */

using namespace mono::interp;

InterpMethod *
mono_interp_get_imethod (MonoDomain *domain, MonoMethod *method, MonoError *error)
{
	if (MonoMethod *replacement = mono::method_override_for (domain, method))
		method = replacement;

	return mono_interp_imethod_named (domain, method, error);
}

InterpMethod *
mono_interp_imethod_named (MonoDomain *domain, MonoMethod *method, MonoError *error)
{
	InterpMethod *imethod;
	MonoMethodSignature *sig;
	int i;

	error_init (error);

	if (InterpMethod *known = lookup_imethod (domain, method))
		return known;

	/*
	 * One body for every reference instantiation, and every instantiation's
	 * record names it. So the tier counter and the promotion request below
	 * belong to the shared form, which is the method a compile is asked for.
	 *
	 * The recursion ends at the shared form itself: it is open, and
	 * shared_form () answers NULL for an open method.
	 */
	if (MonoMethod *shared = shared_form (method)) {
		InterpMethod *body = mono_interp_imethod_named (domain, shared, error);

		if (!is_ok (error))
			return NULL;

		llvm::Expected<mono::MonoDomainMethod *> instantiation =
			mono::domain_method_get (domain, method);

		if (!instantiation) {
			mono_error_set_execution_engine (
				error, "%s", llvm::toString (instantiation.takeError ()).c_str ());
			return NULL;
		}

		return (*instantiation)->set_interp_method (body);
	}

	llvm::Expected<mono::MonoDomainMethod *> dm = mono::domain_method_get (domain, method);

	if (!dm) {
		mono_error_set_execution_engine (error, "%s",
		                                 llvm::toString (dm.takeError ()).c_str ());
		return NULL;
	}

	if (InterpMethod *known = (*dm)->interp_method ())
		return known;

	sig = mono_method_signature_internal (method);

	imethod = static_cast<InterpMethod *> (m_method_alloc0 (domain, method, sizeof (InterpMethod)));
	imethod->method = method;
	imethod->domain = domain;
	imethod->param_count = sig->param_count;
	imethod->hasthis = sig->hasthis;
	imethod->vararg = sig->call_convention == MONO_CALL_VARARG;
	imethod->code_type = IMETHOD_CODE_UNKNOWN;
	if (imethod->method->string_ctor)
		imethod->rtype = m_class_get_byval_arg (mono_defaults.string_class);
	else
		imethod->rtype = mini_get_underlying_type (sig->ret);
	imethod->code_owner = mono_method_get_code_owner_handle (domain, method);
	arm_tier_counter (imethod, (*dm)->tier_calls.load (std::memory_order_relaxed));
	imethod->param_types = static_cast<MonoType **> (
		m_method_alloc0 (domain, method, sizeof (MonoType *) * sig->param_count));
	for (i = 0; i < sig->param_count; ++i)
		imethod->param_types[i] = mini_get_underlying_type (sig->params[i]);

	imethod->prof_flags = mono_profiler_get_call_instrumentation_flags (imethod->method);
#ifdef ENABLE_INTERP_TRACE
	imethod->tracing = trace_wants_method (method);
#endif

	/* Published last, so no other thread can find one that is not filled in. */
	return (*dm)->set_interp_method (imethod);
}
