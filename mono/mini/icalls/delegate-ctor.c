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

/* Delegate construction with a method resolved by the compiler. */
void
ves_icall_mono_delegate_ctor_with_method (MonoObject *this_obj_raw, MonoObject *target_raw,
                                          gpointer addr, MonoMethod *method)
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

	/* Reject a null target for a closed instance-method binding. */
	if (MONO_HANDLE_IS_NULL (target) && !(method->flags & METHOD_ATTRIBUTE_STATIC)) {
		MonoMethod *invoke = mono_get_delegate_invoke_internal (mono_handle_class (this_obj));
		MonoMethodSignature *invoke_sig = invoke ? mono_method_signature_internal (invoke) : NULL;
		MonoMethodSignature *bound_sig = mono_method_signature_internal (method);

		if (invoke_sig && bound_sig && invoke_sig->param_count == bound_sig->param_count) {
			mono_error_set_argument (error, "this", "Delegate to an instance method cannot have null 'this'");
			mono_error_set_pending_exception (error);
			goto leave;
		}
	}

	mono_delegate_ctor (this_obj, target, addr, method, error);
	mono_error_set_pending_exception (error);

leave:
	HANDLE_FUNCTION_RETURN ();
}
