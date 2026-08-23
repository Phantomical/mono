/**
 * \file
 * ldtoken: the runtime handle a metadata token stands for.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

gpointer
mono_ldtoken_wrapper (MonoImage *image, int token, MonoGenericContext *context)
{
	ERROR_DECL (error);
	MonoClass *handle_class;
	gpointer res;

	res = mono_ldtoken_checked (image, token, &handle_class, context, error);
	if (!is_ok (error)) {
		mono_error_set_pending_exception (error);
		return NULL;
	}
	mono_class_init_internal (handle_class);

	return res;
}

gpointer
mono_ldtoken_wrapper_generic_shared (MonoImage *image, int token, MonoMethod *method)
{
	MonoMethodSignature *sig = mono_method_signature_internal (method);
	MonoGenericContext *generic_context;

	if (sig->is_inflated) {
		generic_context = mono_method_get_context (method);
	} else {
		MonoGenericContainer *generic_container = mono_method_get_generic_container (method);
		g_assert (generic_container);
		generic_context = &generic_container->context;
	}

	return mono_ldtoken_wrapper (image, token, generic_context);
}
