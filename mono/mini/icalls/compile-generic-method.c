/**
 * \file
 * The target of a generic virtual call, compiled on demand.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

gpointer
mono_helper_compile_generic_method (MonoObject *obj, MonoMethod *method, gpointer *this_arg)
{
	ERROR_DECL (error);
	MonoMethod *vmethod;
	gpointer addr;
	MonoGenericContext *context = mono_method_get_context (method);

	UnlockedIncrement (&mono_jit_stats.generic_virtual_invocations);

	if (obj == NULL) {
		mono_error_set_null_reference (error);
		mono_error_set_pending_exception (error);
		return NULL;
	}
	vmethod = mono_object_get_virtual_method_internal (obj, method);
	g_assert (!mono_class_is_gtd (vmethod->klass));
	g_assert (!mono_class_is_ginst (vmethod->klass) || !mono_class_get_generic_class (vmethod->klass)->context.class_inst->is_open);
	g_assert (!context->method_inst || !context->method_inst->is_open);

	addr = mono_compile_method_checked (vmethod, error);
	if (mono_error_set_pending_exception (error))
		return NULL;
	g_assert (addr);

	addr = mini_add_method_trampoline (vmethod, addr, FALSE);

	/* Since this is a virtual call, have to unbox vtypes */
	if (m_class_is_valuetype (obj->vtable->klass))
		*this_arg = mono_object_unbox_internal (obj);
	else
		*this_arg = obj;

	return addr;
}
