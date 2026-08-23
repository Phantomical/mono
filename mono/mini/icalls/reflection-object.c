/**
 * \file
 * The reflection object for an assembly or a method.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

MonoObject*
mono_get_assembly_object (MonoImage *image)
{
	ICALL_ENTRY();
	MonoObjectHandle result = MONO_HANDLE_CAST (MonoObject, mono_assembly_get_object_handle (mono_domain_get (), image->assembly, error));
	ICALL_RETURN_OBJ (result);
}

MonoObject*
mono_get_method_object (MonoMethod *method)
{
	ERROR_DECL (error);
	MonoObject * result;
	result = (MonoObject*)mono_method_get_object_checked (mono_domain_get (), method, method->klass, error);
	mono_error_set_pending_exception (error);
	return result;
}
