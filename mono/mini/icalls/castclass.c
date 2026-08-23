/**
 * \file
 * castclass and isinst, where the inline test in generated code did not settle it.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

MonoObject*
mono_object_castclass_unbox (MonoObject *obj, MonoClass *klass)
{
	ERROR_DECL (error);
	MonoJitTlsData *jit_tls = NULL;
	MonoClass *oklass;

	if (mini_debug_options.better_cast_details) {
		jit_tls = mono_tls_get_jit_tls ();
		jit_tls->class_cast_from = NULL;
	}

	if (!obj)
		return NULL;

	oklass = obj->vtable->klass;
	if ((m_class_is_enumtype (klass) && oklass == m_class_get_element_class (klass)) || (m_class_is_enumtype (oklass) && klass == m_class_get_element_class (oklass)))
		return obj;
	if (mono_object_isinst_checked (obj, klass, error))
		return obj;
	if (mono_error_set_pending_exception (error))
		return NULL;

	if (mini_debug_options.better_cast_details) {
		jit_tls->class_cast_from = oklass;
		jit_tls->class_cast_to = klass;
	}

	mono_set_pending_exception (mono_exception_from_name (mono_defaults.corlib,
					"System", "InvalidCastException"));

	return NULL;
}

/*
 * A transparent proxy never gets into the cache. Answering the cast for one is not a
 * pure test: mono_object_isinst_checked () asks the proxy's CanCastTo () and then
 * mono_upgrade_remote_class () gives the object a new vtable carrying the interface it
 * just admitted to. Caching the vtable it had on the way in would let the next proxy
 * short-circuit to "yes" without ever being upgraded, and the call that follows would
 * dispatch through an IMT slot nobody filled. Since a proxy's vtable is only ever a
 * proxy's, keeping them all out of the cache also keeps one out of the fast path above.
 */
MonoObject*
mono_object_castclass_with_cache (MonoObject *obj, MonoClass *klass, gpointer *cache)
{
	ERROR_DECL (error);
	MonoJitTlsData *jit_tls = NULL;
	gpointer cached_vtable, obj_vtable;

	if (mini_debug_options.better_cast_details) {
		jit_tls = mono_tls_get_jit_tls ();
		jit_tls->class_cast_from = NULL;
	}

	if (!obj)
		return NULL;

	cached_vtable = *cache;
	obj_vtable = obj->vtable;

	if (cached_vtable == obj_vtable)
		return obj;

	if (mono_object_isinst_checked (obj, klass, error)) {
		if (!mono_object_is_transparent_proxy (obj))
			*cache = obj_vtable;
		return obj;
	}
	if (mono_error_set_pending_exception (error))
		return NULL;

	if (mini_debug_options.better_cast_details) {
		jit_tls->class_cast_from = obj->vtable->klass;
		jit_tls->class_cast_to = klass;
	}

	mono_set_pending_exception (mono_exception_from_name (mono_defaults.corlib,
					"System", "InvalidCastException"));

	return NULL;
}

/* Excludes transparent proxies for the reason mono_object_castclass_with_cache () gives,
 * the negative cache included - a proxy's CanCastTo () may well say yes to the next one. */
MonoObject*
mono_object_isinst_with_cache (MonoObject *obj, MonoClass *klass, gpointer *cache)
{
	ERROR_DECL (error);
	size_t cached_vtable, obj_vtable;

	if (!obj)
		return NULL;

	cached_vtable = (size_t)*cache;
	obj_vtable = (size_t)obj->vtable;

	if ((cached_vtable & ~0x1) == obj_vtable) {
		return (cached_vtable & 0x1) ? NULL : obj;
	}

	if (mono_object_isinst_checked (obj, klass, error)) {
		if (!mono_object_is_transparent_proxy (obj))
			*cache = (gpointer)obj_vtable;
		return obj;
	} else {
		if (mono_error_set_pending_exception (error))
			return NULL;
		/*negative cache*/
		if (!mono_object_is_transparent_proxy (obj))
			*cache = (gpointer)(obj_vtable | 0x1);
		return NULL;
	}
}
