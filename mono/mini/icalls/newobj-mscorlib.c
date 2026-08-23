/**
 * \file
 * A new instance of a corlib class named by its type-def index.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

MonoObject*
mono_helper_newobj_mscorlib (guint32 idx)
{
	ERROR_DECL (error);
	MonoClass *klass = mono_class_get_checked (mono_defaults.corlib, MONO_TOKEN_TYPE_DEF | idx, error);

	if (!is_ok (error)) {
		mono_error_set_pending_exception (error);
		return NULL;
	}

	MonoObject *obj = mono_object_new_checked (mono_domain_get (), klass, error);
	if (!is_ok (error))
		mono_error_set_pending_exception (error);
	return obj;
}
