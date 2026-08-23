#include "config.h"
#include "mono/interp/runtime/method.hpp"

#include "mintops.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/gc-internals.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/object.h"
#include "mono/interp/interp.hpp"
#include "mono/utils/mono-error-internals.h"
#include <cstring>

namespace mono::interp {

/*
 * newobj allocates the object and then calls a constructor, which returns void.
 * The object therefore has to be in two places when the call is made: at the call
 * arguments, as the constructor's this, and one slot below, where the value the
 * newobj produces is read from once the constructor returns.
 *
 * Arguments the transform already staged sit where the two copies go, so they move
 * up first. MINT_NEWOBJ_FAST and MINT_NEWOBJ_VT_FAST carry an INLINED_METHOD_FLAG in the
 * method slot when the transform inlined the constructor body. They then dispatch on the
 * inlined code instead of calling it.
 */

MONO_INTERP_OP_IMPL (MINT_NEWOBJ)
{
	guint16 param_size = ip[3];
	call_args_offset = ip[1];

	cmethod = static_cast<InterpMethod *> (frame->imethod->data_items[ip[2]]);

	if (param_size)
		std::memmove (locals + call_args_offset + 2 * MINT_STACK_SLOT_SIZE,
		              locals + call_args_offset, param_size);

	MonoClass *newobj_class = cmethod->method->klass;

	g_assert (!m_class_is_valuetype (newobj_class));

	MonoDomain *domain = frame->imethod->domain;

	error_init_reuse (error);
	MonoVTable *vtable = mono_class_vtable_checked (domain, newobj_class, error);
	if (!is_ok (error) || !mono_runtime_class_init_full (vtable, error)) {
		MonoException *exc = mono_error_convert_to_exception (error);
		g_assert (exc);
		THROW_EX (exc, ip);
	}

	error_init_reuse (error);
	MonoObject *o = mono_object_new_checked (domain, newobj_class, error);
	LOCAL_VAR (call_args_offset, MonoObject *) = o; // return value
	call_args_offset += MINT_STACK_SLOT_SIZE;
	LOCAL_VAR (call_args_offset, MonoObject *) = o; // first parameter

	mono_interp_error_cleanup (error); // FIXME: do not swallow the error
	EXCEPTION_CHECKPOINT;

#ifndef DISABLE_REMOTING
	if (mono_object_is_transparent_proxy (o)) {
		MonoMethod *remoting_invoke_method =
			mono_marshal_get_remoting_invoke_with_check (cmethod->method, error);
		mono_error_assert_ok (error);
		cmethod = mono_interp_get_imethod (domain, remoting_invoke_method, error);
		mono_error_assert_ok (error);
	}
#endif

	MONO_INTERP_OP_ADVANCE ();
	return &exec_call;
}

/*
 * The constructor arrives in a local rather than a data item, because a body
 * shared between reference instantiations reads it out of its generic context.
 * The class, its vtable and its initializer all come off that constructor, so
 * this is the whole of what the context has to answer.
 */
MONO_INTERP_OP_IMPL (MINT_NEWOBJ_DYN)
{
	guint16 param_size = ip[3];
	call_args_offset = ip[1];

	cmethod = LOCAL_VAR (ip[2], InterpMethod *);

	if (param_size)
		std::memmove (locals + call_args_offset + 2 * MINT_STACK_SLOT_SIZE,
		              locals + call_args_offset, param_size);

	MonoClass *newobj_class = cmethod->method->klass;

	g_assert (!m_class_is_valuetype (newobj_class));

	MonoDomain *domain = frame->imethod->domain;

	error_init_reuse (error);
	MonoVTable *vtable = mono_class_vtable_checked (domain, newobj_class, error);
	if (!is_ok (error) || !mono_runtime_class_init_full (vtable, error)) {
		MonoException *exc = mono_error_convert_to_exception (error);
		g_assert (exc);
		THROW_EX (exc, ip);
	}

	error_init_reuse (error);
	MonoObject *o = mono_object_new_checked (domain, newobj_class, error);
	LOCAL_VAR (call_args_offset, MonoObject *) = o; // return value
	call_args_offset += MINT_STACK_SLOT_SIZE;
	LOCAL_VAR (call_args_offset, MonoObject *) = o; // first parameter

	mono_interp_error_cleanup (error); // FIXME: do not swallow the error
	EXCEPTION_CHECKPOINT;

#ifndef DISABLE_REMOTING
	if (mono_object_is_transparent_proxy (o)) {
		MonoMethod *remoting_invoke_method =
			mono_marshal_get_remoting_invoke_with_check (cmethod->method, error);
		mono_error_assert_ok (error);
		cmethod = mono_interp_get_imethod (domain, remoting_invoke_method, error);
		mono_error_assert_ok (error);
	}
#endif

	MONO_INTERP_OP_ADVANCE ();
	return &exec_call;
}

MONO_INTERP_OP_IMPL (MINT_NEWOBJ_FAST)
{
	auto vtable = static_cast<MonoVTable *> (frame->imethod->data_items[ip[3]]);
	INIT_VTABLE (vtable);

	guint16 imethod_index = ip[2];
	guint16 param_size = ip[4];
	call_args_offset = ip[1];
	bool is_inlined = imethod_index == INLINED_METHOD_FLAG;

	// Make room for two copies of o -- this parameter and return value.
	if (param_size)
		std::memmove (locals + call_args_offset + 2 * MINT_STACK_SLOT_SIZE,
		              locals + call_args_offset, param_size);

	MonoObject *o = mono_gc_alloc_obj (vtable, m_class_get_instance_size (vtable->klass));
	if (G_UNLIKELY (!o)) {
		error_init_reuse (error);
		mono_error_set_out_of_memory (error, "Could not allocate %i bytes",
		                              m_class_get_instance_size (vtable->klass));
		THROW_EX (mono_error_convert_to_exception (error), ip);
	}

	// This is the return value
	LOCAL_VAR (call_args_offset, MonoObject *) = o;
	// Set the `this` argument for the constructor call
	call_args_offset += MINT_STACK_SLOT_SIZE;
	LOCAL_VAR (call_args_offset, MonoObject *) = o;

	MONO_INTERP_OP_ADVANCE ();

	if (is_inlined)
		MONO_INTERP_DISPATCH ();

	cmethod = static_cast<InterpMethod *> (frame->imethod->data_items[imethod_index]);
	return &exec_call;
}

MONO_INTERP_OP_IMPL (MINT_NEWOBJ_VT_FAST)
{
	guint16 imethod_index = ip[2];
	guint16 ret_size = ip[3];
	guint16 param_size = ip[4];
	bool is_inlined = imethod_index == INLINED_METHOD_FLAG;
	call_args_offset = ip[1];
	gpointer this_vt = locals + call_args_offset;

	if (param_size)
		std::memmove (locals + call_args_offset + ret_size + MINT_STACK_SLOT_SIZE,
		              locals + call_args_offset, param_size);

	// clear the valuetype
	std::memset (this_vt, 0, ret_size);
	call_args_offset += ret_size;
	// pass the address of the valuetype
	LOCAL_VAR (call_args_offset, gpointer) = this_vt;

	MONO_INTERP_OP_ADVANCE ();

	if (is_inlined)
		MONO_INTERP_DISPATCH ();

	cmethod = static_cast<InterpMethod *> (frame->imethod->data_items[imethod_index]);
	return &exec_call;
}

MONO_INTERP_OP_IMPL (MINT_NEWOBJ_STRING)
{
	cmethod = static_cast<InterpMethod *> (frame->imethod->data_items[ip[2]]);
	call_args_offset = ip[1];

	int param_size = ip[3];
	if (param_size)
		std::memmove (locals + call_args_offset + MINT_STACK_SLOT_SIZE, locals + call_args_offset,
		              param_size);

	// `this` is implicit null. The created string is returned by the call, even
	// though the call has a void return type.
	LOCAL_VAR (call_args_offset, gpointer) = nullptr;

	MONO_INTERP_OP_ADVANCE ();
	return &exec_call;
}

MONO_INTERP_OP_IMPL (MINT_MONO_NEWOBJ)
{
	auto klass = static_cast<MonoClass *> (frame->imethod->data_items[ip[2]]);

	error_init_reuse (error);
	LOCAL_VAR (ip[1], MonoObject *) =
		mono_object_new_checked (frame->imethod->domain, klass, error);
	mono_interp_error_cleanup (error); // FIXME: do not swallow the error

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
