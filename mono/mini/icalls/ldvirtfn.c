/**
 * \file
 * ldvirtftn: the entry point a virtual call on the receiver reaches.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

static void*
ldvirtfn_internal (MonoObject *obj, MonoMethod *method, gboolean gshared)
{
	ERROR_DECL (error);
	MonoMethod *res;
	gpointer addr;

	if (obj == NULL) {
		mono_error_set_null_reference (error);
		mono_error_set_pending_exception (error);
		return NULL;
	}

	res = mono_object_get_virtual_method_internal (obj, method);

	if (gshared && method->is_inflated && mono_method_get_context (method)->method_inst) {
		MonoGenericContext context = { NULL, NULL };

		if (mono_class_is_ginst (res->klass))
			context.class_inst = mono_class_get_generic_class (res->klass)->context.class_inst;
		else if (mono_class_is_gtd (res->klass))
			context.class_inst = mono_class_get_generic_container (res->klass)->context.class_inst;
		context.method_inst = mono_method_get_context (method)->method_inst;

		res = mono_class_inflate_generic_method_checked (res, &context, error);
		if (!is_ok (error)) {
			mono_error_set_pending_exception (error);
			return NULL;
		}
	}

	/* An rgctx wrapper is added by the trampolines no need to do it here */
	gboolean need_unbox = m_class_is_valuetype (res->klass) && !m_class_is_valuetype (method->klass);
	if (need_unbox) {
		/*
		 * We can't return a jump trampoline here, because the trampoline code
		 * can't determine whenever to add an unbox trampoline (ldvirtftn) or
		 * not (ldftn). So compile the method here.
		 */
		addr = mono_compile_method_checked (res, error);
		if (!is_ok (error)) {
			mono_error_set_pending_exception (error);
			return NULL;
		}

		if (mono_llvm_only && mono_method_needs_static_rgctx_invoke (res, FALSE))
			// FIXME:
			g_assert_not_reached ();

		addr = mini_add_method_trampoline (res, addr, TRUE);
	} else {
		addr = mono_ldftn (res);
	}
	return addr;
}

void*
mono_ldvirtfn (MonoObject *obj, MonoMethod *method)
{
	return ldvirtfn_internal (obj, method, FALSE);
}

void*
mono_ldvirtfn_gshared (MonoObject *obj, MonoMethod *method)
{
	return ldvirtfn_internal (obj, method, TRUE);
}
