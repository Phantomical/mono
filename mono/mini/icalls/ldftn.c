/**
 * \file
 * ldftn: the entry point of a method, as a function pointer.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"
#include <mono/metadata/marshal.h>

void*
mono_ldftn (MonoMethod *method)
{
	gpointer addr;
	ERROR_DECL (error);

	/*
	 * A synchronized method's lock is in the wrapper rather than in the body.
	 * Whoever calls through this pointer has no other chance to take it.
	 */
	if (method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
		method = mono_marshal_get_synchronized_wrapper (method);

	/*
	 * mono_create_jump_trampoline ()'s stub is callable but is not the
	 * method's published thunk. mono_compile_method_checked () resolves that
	 * thunk directly, the way GetFunctionPointer () and the interpreter's
	 * native_entry_for_imethod () already do, so ldftn's product agrees with
	 * theirs.
	 */
	addr = mono_compile_method_checked (method, error);
	mono_error_assert_ok (error);
	g_assert (addr);

	addr = mini_add_method_trampoline (method, addr, FALSE);
	return addr;
}
