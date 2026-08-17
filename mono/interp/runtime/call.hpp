#ifndef __MONO_INTERP_INTERP_CALL_HPP__
#define __MONO_INTERP_INTERP_CALL_HPP__

/**
 * \file
 * \brief What a call site works out before it hands over a frame.
 */

#include "internals.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/object-internals.h"
#include "mono/utils/atomic.h"
#include "mono/utils/mono-error-internals.h"
#include "mono/utils/mono-memory-model.h"

#include <memory>

namespace mono::interp {

inline InterpMethod *
get_virtual_method (InterpMethod *imethod, MonoVTable *vtable)
{
	MonoMethod *m = imethod->method;
	MonoDomain *domain = imethod->domain;
	InterpMethod *ret = NULL;

#ifndef DISABLE_REMOTING
	if (mono_class_is_transparent_proxy (vtable->klass)) {
		ERROR_DECL (error);
		MonoMethod *remoting_invoke_method = mono_marshal_get_remoting_invoke_with_check (m, error);
		mono_error_assert_ok (error);
		ret = mono_interp_get_imethod (domain, remoting_invoke_method, error);
		mono_error_assert_ok (error);
		return ret;
	}
#endif

	if ((m->flags & METHOD_ATTRIBUTE_FINAL) || !(m->flags & METHOD_ATTRIBUTE_VIRTUAL)) {
		if (m->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED) {
			ERROR_DECL (error);
			ret =
				mono_interp_get_imethod (domain, mono_marshal_get_synchronized_wrapper (m), error);
			mono_interp_error_cleanup (error); /* FIXME: don't swallow the error */
		} else {
			ret = imethod;
		}
		return ret;
	}

	mono_class_setup_vtable (vtable->klass);

	int slot = mono_method_get_vtable_slot (m);
	if (mono_class_is_interface (m->klass)) {
		g_assert (vtable->klass != m->klass);
		/* TODO: interface offset lookup is slow, go through IMT instead */
		gboolean non_exact_match;
		slot +=
			mono_class_interface_offset_with_variance (vtable->klass, m->klass, &non_exact_match);
	}

	MonoMethod *virtual_method = m_class_get_vtable (vtable->klass)[slot];
	if (m->is_inflated && mono_method_get_context (m)->method_inst) {
		MonoGenericContext context = {NULL, NULL};

		if (mono_class_is_ginst (virtual_method->klass))
			context.class_inst =
				mono_class_get_generic_class (virtual_method->klass)->context.class_inst;
		else if (mono_class_is_gtd (virtual_method->klass))
			context.class_inst =
				mono_class_get_generic_container (virtual_method->klass)->context.class_inst;
		context.method_inst = mono_method_get_context (m)->method_inst;

		ERROR_DECL (error);
		virtual_method =
			mono_class_inflate_generic_method_checked (virtual_method, &context, error);
		mono_error_cleanup (error); /* FIXME: don't swallow the error */
	}

	if (virtual_method->iflags & METHOD_IMPL_ATTRIBUTE_INTERNAL_CALL) {
		virtual_method = mono_marshal_get_native_wrapper (virtual_method, FALSE, FALSE);
	}

	if (virtual_method->iflags & METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED) {
		virtual_method = mono_marshal_get_synchronized_wrapper (virtual_method);
	}

	ERROR_DECL (error);
	InterpMethod *virtual_imethod = mono_interp_get_imethod (domain, virtual_method, error);
	mono_error_cleanup (error); /* FIXME: don't swallow the error */
	return virtual_imethod;
}

/*
 * A method caches its argument offsets on the first ask, because answering for one
 * index means walking every parameter ahead of it.
 */

/* Does not handle `this` argument */
inline guint32
compute_arg_offset (MonoMethodSignature *sig, int index, int prev_offset)
{
	if (index == 0)
		return 0;

	if (prev_offset == -1) {
		guint32 offset = 0;
		for (int i = 0; i < index; i++) {
			int size, align;
			MonoType *type = sig->params[i];
			size = mono_type_size (type, &align);
			offset += ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
		return offset;
	} else {
		int size, align;
		MonoType *type = sig->params[index - 1];
		size = mono_type_size (type, &align);
		return prev_offset + ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
	}
}

inline guint32 *
initialize_arg_offsets (InterpMethod *imethod)
{
	if (imethod->arg_offsets)
		return imethod->arg_offsets;

	MonoMethodSignature *sig = mono_method_signature_internal (imethod->method);
	int arg_count = sig->hasthis + sig->param_count;
	g_assert (arg_count);
	auto arg_offsets = std::make_unique<guint32[]> (arg_count);
	int index = 0, offset_addend = 0, prev_offset = 0;

	if (sig->hasthis) {
		arg_offsets[index++] = 0;
		offset_addend = MINT_STACK_SLOT_SIZE;
	}

	for (int i = 0; i < sig->param_count; i++) {
		prev_offset = compute_arg_offset (sig, i, prev_offset);
		arg_offsets[index++] = prev_offset + offset_addend;
	}

	mono_memory_write_barrier ();
	/* Two threads can reach this for the same method. The winner hands its table
	 * to the method, which holds it for as long as the domain lives and never
	 * gives it back; the loser still owns the one it built, and drops it here. */
	if (mono_atomic_cas_ptr (reinterpret_cast<gpointer *> (&imethod->arg_offsets), arg_offsets.get (), NULL) == NULL)
		arg_offsets.release ();
	return imethod->arg_offsets;
}

inline guint32
get_arg_offset_fast (InterpMethod *imethod, int index)
{
	guint32 *arg_offsets = imethod->arg_offsets;
	if (arg_offsets)
		return arg_offsets[index];

	arg_offsets = initialize_arg_offsets (imethod);
	g_assert (arg_offsets);
	return arg_offsets[index];
}

inline guint32
get_arg_offset (InterpMethod *imethod, MonoMethodSignature *sig, int index)
{
	/*
	 * A managed-to-native wrapper is entered with one signature and calls out with
	 * another, and at the native call the frame holds the marshalled values, laid
	 * out under the second. The cached offsets describe the first only, so they
	 * answer for the signature they were built from and nothing else. HandleRef is
	 * where the difference shows: two words as the managed type, one word once the
	 * wrapper has extracted the handle.
	 */
	if (imethod && sig == mono_method_signature_internal (imethod->method))
		return get_arg_offset_fast (imethod, index);

	g_assert (!sig->hasthis);
	return compute_arg_offset (sig, index, -1);
}

} // namespace mono::interp

#endif
