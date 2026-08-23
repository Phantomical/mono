/**
 * \file
 * The class initializer a body runs before it touches a class's statics.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

void
ves_icall_runtime_class_init (MonoVTable *vtable)
{
	MONO_REQ_GC_UNSAFE_MODE;
	ERROR_DECL (error);

	mono_runtime_class_init_full (vtable, error);
	mono_error_set_pending_exception (error);
}

void
mono_generic_class_init (MonoVTable *vtable)
{
	ERROR_DECL (error);
	mono_runtime_class_init_full (vtable, error);
	mono_error_set_pending_exception (error);
}
