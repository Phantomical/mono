/**
 * \file
 * A constrained call out of a gsharedvt body, dispatched at run time.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

static MonoMethod*
constrained_gsharedvt_call_setup (gpointer mp, MonoMethod *cmethod, MonoClass *klass, gpointer *this_arg, MonoError *error)
{
	MonoMethod *m;
	int vt_slot, iface_offset;
	gboolean is_iface = FALSE;

	error_init (error);

	if (mono_class_is_interface (klass) || !m_class_is_valuetype (klass)) {
		MonoObject *this_obj;

		is_iface = mono_class_is_interface (klass);

		/* Have to use the receiver's type instead of klass, the receiver is a ref type */
		this_obj = *(MonoObject**)mp;
		g_assert (this_obj);

		klass = this_obj->vtable->klass;
	}

	if (mono_method_signature_internal (cmethod)->pinvoke) {
		/* Object.GetType () */
		m = mono_marshal_get_native_wrapper (cmethod, TRUE, FALSE);
	} else {
		/* Lookup the virtual method */
		mono_class_setup_vtable (klass);
		g_assert (m_class_get_vtable (klass));
		vt_slot = mono_method_get_vtable_slot (cmethod);
		if (mono_class_is_interface (cmethod->klass)) {
			iface_offset = mono_class_interface_offset (klass, cmethod->klass);
			g_assert (iface_offset != -1);
			vt_slot += iface_offset;
		}
		m = m_class_get_vtable (klass) [vt_slot];
		if (cmethod->is_inflated) {
			m = mono_class_inflate_generic_method_full_checked (m, NULL, mono_method_get_context (cmethod), error);
			return_val_if_nok (error, NULL);
		}
	}

	if (m_class_is_valuetype (klass) && (m->klass == mono_defaults.object_class || m->klass == m_class_get_parent (mono_defaults.enum_class) || m->klass == mono_defaults.enum_class)) {
		/*
		 * Calling a non-vtype method with a vtype receiver, has to box.
		 */
		*this_arg = mono_value_box_checked (mono_domain_get (), klass, mp, error);
	} else if (m_class_is_valuetype (klass)) {
		if (is_iface) {
			/*
			 * The original type is an interface, so the receiver is a ref,
			   the called method is a vtype method, need to unbox.
			*/
			MonoObject *this_obj = *(MonoObject**)mp;

			*this_arg = mono_object_unbox_internal (this_obj);
		} else {
			/*
			 * Calling a vtype method with a vtype receiver
			 */
			*this_arg = mp;
		}
	} else {
		/*
		 * Calling a non-vtype method
		 */
		*this_arg = *(gpointer*)mp;
	}

	return m;
}

/*
 * mono_gsharedvt_constrained_call:
 *
 *   Make a call to CMETHOD using the receiver MP, which is assumed to be of type KLASS. ARGS contains
 * the arguments to the method in the format used by mono_runtime_invoke_checked ().
 */
MonoObject*
mono_gsharedvt_constrained_call (gpointer mp, MonoMethod *cmethod, MonoClass *klass, gboolean deref_arg, gpointer *args)
{
	ERROR_DECL (error);
	MonoObject *o;
	MonoMethod *m;
	gpointer this_arg;
	gpointer new_args [16];

#ifdef ENABLE_NETCORE
	/* Object.GetType () is an intrinsic under netcore */
	if (!mono_class_is_ginst (cmethod->klass) && !cmethod->is_inflated && !strcmp (cmethod->name, "GetType")) {
		MonoVTable *vt;

		vt = mono_class_vtable_checked (mono_domain_get (), klass, error);
		if (!is_ok (error)) {
			mono_error_set_pending_exception (error);
			return NULL;
		}
		return vt->type;
	}
#endif

	m = constrained_gsharedvt_call_setup (mp, cmethod, klass, &this_arg, error);
	if (!is_ok (error)) {
		mono_error_set_pending_exception (error);
		return NULL;
	}

	if (!m)
		return NULL;
	if (args && deref_arg) {
		new_args [0] = *(gpointer*)args [0];
		args = new_args;
	}
	if (m->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE) {
		/* Object.GetType () */
		args = new_args;
		args [0] = this_arg;
		this_arg = NULL;
	}

	o = mono_runtime_invoke_checked (m, this_arg, args, error);
	if (!is_ok (error)) {
		mono_error_set_pending_exception (error);
		return NULL;
	}

	return o;
}
