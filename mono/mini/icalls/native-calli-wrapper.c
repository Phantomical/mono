/**
 * \file
 * The marshalling wrapper a calli to native code goes through.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

gpointer
mono_get_native_calli_wrapper (MonoImage *image, MonoMethodSignature *sig, gpointer func)
{
	ERROR_DECL (error);
	MonoMarshalSpec **mspecs;
	MonoMethodPInvoke piinfo;
	MonoMethod *m;

	mspecs = g_new0 (MonoMarshalSpec*, sig->param_count + 1);
	memset (&piinfo, 0, sizeof (piinfo));

	m = mono_marshal_get_native_func_wrapper (image, sig, &piinfo, mspecs, func);

	for (int i = sig->param_count; i >= 0; i--)
		if (mspecs [i])
			mono_metadata_free_marshal_spec (mspecs [i]);
	g_free (mspecs);

	gpointer compiled_ptr = mono_compile_method_checked (m, error);
	mono_error_set_pending_exception (error);
	g_assert (compiled_ptr);

	return compiled_ptr;
}
