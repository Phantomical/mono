/**
 * \file
 * The store check stelem.ref makes before the store itself.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

void
mono_helper_stelem_ref_check (MonoArray *array, MonoObject *val)
{
	ERROR_DECL (error);
	if (!array) {
		mono_error_set_null_reference (error);
		mono_error_set_pending_exception (error);
		return;
	}
	if (val && !mono_object_isinst_checked (val, m_class_get_element_class (mono_object_class (array)), error)) {
		if (mono_error_set_pending_exception (error))
			return;
		mono_set_pending_exception (mono_get_exception_array_type_mismatch ());
		return;
	}
}
