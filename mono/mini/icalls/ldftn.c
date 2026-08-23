/**
 * \file
 * ldftn: the entry point of a method, as a function pointer.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

void*
mono_ldftn (MonoMethod *method)
{
	gpointer addr;
	ERROR_DECL (error);

	if (mono_llvm_only) {
		// FIXME: No error handling

		addr = mono_compile_method_checked (method, error);
		mono_error_assert_ok (error);
		g_assert (addr);

		if (mono_method_needs_static_rgctx_invoke (method, FALSE))
			/* The caller doesn't pass it */
			g_assert_not_reached ();

		addr = mini_add_method_trampoline (method, addr, FALSE);
		return addr;
	}

	/* if we need the address of a native-to-managed wrapper, just compile it now, trampoline needs thread local
	 * variables that won't be there if we run on a thread that's not attached yet. */
	if (method->wrapper_type == MONO_WRAPPER_NATIVE_TO_MANAGED) {
		addr = mono_compile_method_checked (method, error);
	} else {
		addr = mono_create_jump_trampoline (mono_domain_get (), method, FALSE, error);
	}
	if (!is_ok (error)) {
		mono_error_set_pending_exception (error);
		return NULL;
	}
	return mono_create_ftnptr (mono_domain_get (), addr);
}
