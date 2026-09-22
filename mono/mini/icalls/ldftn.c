/**
 * \file
 * ldftn: the entry point of a method, as a function pointer.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"
#include <mono/metadata/marshal.h>
#include "../../llvm/runtime.h"

void*
mono_ldftn (MonoMethod *method)
{
	gpointer addr;
	ERROR_DECL (error);

	// A synchronized method's lock is in the wrapper rather than in the body.
	// Whoever calls through this pointer has no other chance to take it.
	if (method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED)
		method = mono_marshal_get_synchronized_wrapper (method);

	/* The backend entry already includes any required rgctx context stub. */
	addr = mono_llvm_jit_stub_for (method, mono_domain_get (), error);
	mono_error_assert_ok (error);
	g_assert (addr);

	return addr;
}
