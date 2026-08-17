/**
 * \file
 * \brief The per-domain table of transformed methods, and its invalidation.
 */

#include "config.h"

#include "callbacks.hpp"
#include "internals.hpp"
#include "method.hpp"
#include "trace.hpp"
#include "entry.hpp"
#include "imethod.hpp"

#include <mono/metadata/marshal.h>
#include <mono/metadata/metadata-update.h>
#include <mono/mini/mini-runtime.h>
#include <mono/utils/mono-logger-internals.h>
#include <mono/utils/mono-threads.h>

namespace mono::interp {

InterpMethod *
lookup_imethod (MonoDomain *domain, MonoMethod *method)
{
	InterpMethod *imethod;
	MonoJitDomainInfo *info;

	info = domain_jit_info (domain);
	mono_domain_jit_code_hash_lock (domain);
	imethod = (InterpMethod*)mono_internal_hash_table_lookup (&info->interp_code_hash, method);
	mono_domain_jit_code_hash_unlock (domain);
	return imethod;
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

/*
 * The interpreter's record for a method in the current domain, creating one if
 * it has none. Says who would run the method; does not transform it.
 */
void
interp_arm_tier_counter (gpointer imethod, gint32 calls)
{
	mono::interp::arm_tier_counter (imethod, calls);
}

/*
 * interp_transform_method:
 *
 * Transform METHOD now rather than on the first call into it, so that whether
 * the interpreter can run it at all is known before anything can. Returns FALSE
 * with ERROR set when it cannot.
 *
 * This is what create_method_pointer () does for itself when asked to compile,
 * and is separate so that a caller which has to publish the entry first can
 * still find out. Transforming runs the class initializer, so a caller holding
 * a lock that managed code could reach has to get the entry reachable before it
 * asks.
 */
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

void
interp_free_method (MonoDomain *domain, MonoMethod *method)
{
	MonoJitDomainInfo *info = domain_jit_info (domain);

	mono_domain_jit_code_hash_lock (domain);
	/* InterpMethod is allocated in the domain mempool. We might haven't
	 * allocated an InterpMethod for this instance yet */
	mono_internal_hash_table_remove (&info->interp_code_hash, method);
	mono_domain_jit_code_hash_unlock (domain);
}

/*
 * METHOD now has native code in DOMAIN, so calls to it from interpreted code go
 * there instead of interpreting it. Nothing is created here: a method the
 * interpreter has never seen settles this for itself the first time it is
 * called, and it can only be settled the other way while there is no code.
 */
void
interp_method_compiled (MonoDomain *domain, MonoMethod *method)
{
	InterpMethod *imethod = lookup_imethod (domain, method);

	if (imethod == NULL || imethod->code_type == IMETHOD_CODE_COMPILED)
		return;

	if (mono_interp_jit_call_marshallable (method, mono_method_signature_internal (method)))
		imethod->code_type = IMETHOD_CODE_COMPILED;
}

/*
 * Takes back the answer resolve_code_type () settled for this method, because the
 * address it is entered at is now in native hands. Native code can write a jump
 * over that address, and an answer taken before that did not know calls have to
 * go through it.
 */
void
interp_entry_escaped (MonoDomain *domain, MonoMethod *method)
{
	InterpMethod *imethod = lookup_imethod (domain, method);

	if (imethod == NULL)
		return;

	mono_atomic_cas_i32 ((gint32*)&imethod->code_type, IMETHOD_CODE_UNKNOWN,
	                     IMETHOD_CODE_INTERP);
}

static void
invalidate_transform (gpointer imethod_)
{
	InterpMethod *imethod = (InterpMethod *) imethod_;
	imethod->transformed = FALSE;
}

static void
copy_imethod_for_frame (MonoDomain *domain, InterpFrame *frame)
{
	InterpMethod *copy = (InterpMethod *) mono_domain_alloc0 (domain, sizeof (InterpMethod));
	memcpy (copy, frame->imethod, sizeof (InterpMethod));
	copy->next_jit_code_hash = NULL; /* we don't want that in our copy */
	frame->imethod = copy;
	/* Note: The copy will be around until the domain is unloading. Ideally we
	 * would reclaim its memory when the corresponding InterpFrame is popped.
	 */
}

void
interp_metadata_update_init (MonoError *error)
{
	if ((mono_interp_opt & INTERP_OPT_INLINE) != 0)
		mono_error_set_execution_engine (error, "Interpreter inlining must be turned off for metadata updates");
}

static void
metadata_update_backup_frames (MonoDomain *domain, MonoThreadInfo *info, InterpFrame *frame)
{
	while (frame) {
		mono_trace (G_LOG_LEVEL_DEBUG, MONO_TRACE_METADATA_UPDATE, "threadinfo=%p, copy imethod for method=%s", info, mono_method_full_name (frame->imethod->method, 1));
		copy_imethod_for_frame (domain, frame);
		frame = frame->parent;
	}
}

static void
metadata_update_prepare_to_invalidate (MonoDomain *domain)
{
	/* (1) make a copy of imethod for every interpframe that is on the stack,
	 * so we do not invalidate currently running methods */

	FOREACH_THREAD_EXCLUDE (info, MONO_THREAD_INFO_FLAGS_NO_GC) {
		if (!info || !info->jit_data)
			continue;

		ThreadContext *context = (ThreadContext*)info->jit_data->interp_context;

		/* If the thread was in the interpreter and hit a safepoint
		 * opcode and suspended, backup the frames since the last lmf.
		 */
		if (context && context->safepoint_frame) {
			mono_trace (G_LOG_LEVEL_DEBUG, MONO_TRACE_METADATA_UPDATE, "threadinfo=%p, has safepoint frame %p", info, context->safepoint_frame);
			metadata_update_backup_frames (domain, info, context->safepoint_frame);
		}

		MonoLMF *lmf = info->jit_data->lmf;
		while (lmf) {
			if (((gsize) lmf->previous_lmf) & 2) {
				MonoLMFExt *ext = (MonoLMFExt *) lmf;
				if (ext->kind == MONO_LMFEXT_INTERP_EXIT || ext->kind == MONO_LMFEXT_INTERP_EXIT_WITH_CTX) {
					InterpFrame *frame = (InterpFrame *) ext->interp_exit_data;
					metadata_update_backup_frames (domain, info, frame);
				}
			}
			lmf = (MonoLMF *)(((gsize) lmf->previous_lmf) & ~3);
		}
	} FOREACH_THREAD_END

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
	MonoJitDomainInfo *info = domain_jit_info (domain);
	mono_domain_jit_code_hash_lock (domain);
	mono_internal_hash_table_apply (&info->interp_code_hash, invalidate_transform);
	mono_domain_jit_code_hash_unlock (domain);

	if (need_stw_restart)
		mono_gc_restart_world ();
}

} // namespace mono::interp

/* Outside the namespace, because interp-internals.hpp declares them there. */

using namespace mono::interp;

InterpMethod*
mono_interp_get_imethod (MonoDomain *domain, MonoMethod *method, MonoError *error)
{
	InterpMethod *imethod;
	MonoJitDomainInfo *info;
	MonoMethodSignature *sig;
	int i;

	error_init (error);

	info = domain_jit_info (domain);
	mono_domain_jit_code_hash_lock (domain);
	imethod = (InterpMethod*)mono_internal_hash_table_lookup (&info->interp_code_hash, method);
	mono_domain_jit_code_hash_unlock (domain);
	if (imethod)
		return imethod;

	sig = mono_method_signature_internal (method);

	imethod = (InterpMethod*)m_method_alloc0 (domain, method, sizeof (InterpMethod));
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
	imethod->param_types = (MonoType**)m_method_alloc0 (domain, method, sizeof (MonoType*) * sig->param_count);
	for (i = 0; i < sig->param_count; ++i)
		imethod->param_types [i] = mini_get_underlying_type (sig->params [i]);

	mono_domain_jit_code_hash_lock (domain);
	if (!mono_internal_hash_table_lookup (&info->interp_code_hash, method))
		mono_internal_hash_table_insert (&info->interp_code_hash, method, imethod);
	mono_domain_jit_code_hash_unlock (domain);

	imethod->prof_flags = mono_profiler_get_call_instrumentation_flags (imethod->method);
#ifdef ENABLE_INTERP_TRACE
	imethod->tracing = trace_wants_method (method);
#endif

	return imethod;
}
