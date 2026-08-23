/**
 * \file
 * The delegate constructor a newobj on a delegate class reaches.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

void
ves_icall_mono_delegate_ctor (MonoObject *this_obj_raw, MonoObject *target_raw, gpointer addr)
{
	HANDLE_FUNCTION_ENTER ();
	ERROR_DECL (error);
	MONO_HANDLE_DCL (MonoObject, this_obj);
	MONO_HANDLE_DCL (MonoObject, target);

	if (!addr) {
		mono_error_set_argument_null (error, "method", "");
		mono_error_set_pending_exception (error);
		goto leave;
	}
	mono_delegate_ctor (this_obj, target, addr, NULL, error);
	mono_error_set_pending_exception (error);

leave:
	HANDLE_FUNCTION_RETURN ();
}
