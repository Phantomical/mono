/**
 * \file
 * The runtime generic context slot a shared body reads, filled on the miss.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

gpointer
mono_fill_class_rgctx (MonoVTable *vtable, int index)
{
	ERROR_DECL (error);
	gpointer res;

	/*
	 * This is perf critical.
	 * fill_runtime_generic_context () contains a fallpath.
	 */
	res = mono_class_fill_runtime_generic_context (vtable, index, error);
	if (!is_ok (error)) {
		mono_error_set_pending_exception (error);
		return NULL;
	}
	return res;
}

gpointer
mono_fill_method_rgctx (MonoMethodRuntimeGenericContext *mrgctx, int index)
{
	ERROR_DECL (error);
	gpointer res;

	res = mono_method_fill_runtime_generic_context (mrgctx, index, error);
	if (!is_ok (error)) {
		mono_error_set_pending_exception (error);
		return NULL;
	}
	return res;
}
