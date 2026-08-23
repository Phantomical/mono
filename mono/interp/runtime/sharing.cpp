/**
 * \file
 * \brief Which methods run one body between their reference instantiations.
 */

#include "config.h"

#include "internals.hpp"
#include "sharing.hpp"

#include <mono/llvm/runtime/options.hpp>
#include <mono/metadata/class-internals.h>
#include <mono/mini/mini.h>
#include <mono/mini/mini-runtime.h>

namespace mono::interp {

/*
 * Reference sharing only. Every number a body burns - a field offset, an array
 * element size, a vtable slot index - is the same for every reference
 * instantiation, because a reference is one pointer whatever it points at. A
 * value type changes all of them, so an instantiation naming one gets a body of
 * its own.
 *
 * SHARE_MODE_NONE is what the compiled tier asks for as well, so a method that
 * shares here shares there. The gates below are the wider set, so the other
 * direction does not hold.
 */
MonoMethod *
shared_form (MonoMethod *method)
{
	if (!mono_class_generic_sharing_enabled (method->klass))
		return nullptr;

	/*
	 * A shared body carries the sequence points and the jit info of the shared
	 * form, so a breakpoint asked for on one instantiation has nothing of that
	 * instantiation's to bind to.
	 */
	if (mini_get_debug_options ()->gen_sdb_seq_points)
		return nullptr;

	// The shared method is itself open, and asking it for its own shared form
	// again is how this would recurse.
	if (mono_method_check_context_used (method) != 0)
		return nullptr;

	if (!mono_method_is_generic_sharable_full (method, FALSE, FALSE, FALSE))
		return nullptr;

	/*
	 * Only the shapes that read their context out of a receiver, which is
	 * every instantiation of an ordinary instance method of a reference
	 * generic class. The rest are entered with the context passed in, and no
	 * interpreter entry carries one yet.
	 */
	if (takes_rgctx_argument (method))
		return nullptr;

	ERROR_DECL (share_error);
	MonoMethod *shared = mini_get_shared_method_full (method, SHARE_MODE_NONE, share_error);

	mono_error_cleanup (share_error);

	if (shared == nullptr || shared == method)
		return nullptr;

	/*
	 * MONO_LLVM_JIT_TRACE rather than a switch of its own: sharing is one
	 * decision that both engines take, and the compiled tier already reports
	 * its half here.
	 */
	if (mono::is_jit_trace_enabled ()) {
		char *from = mono_method_full_name (method, TRUE);
		char *to = mono_method_full_name (shared, TRUE);

		MONO_LOCK (mono::jit_trace_mutex ())
		{
			fprintf (stderr, "[interp] %s shares the body of %s\n", from, to);
		}
		g_free (from);
		g_free (to);
	}

	return shared;
}

bool
depends_on_context (MonoClass *klass)
{
	if (mono_class_is_gtd (klass))
		return false;

	return mono_class_check_context_used (klass) != 0;
}

bool
depends_on_context (MonoClassField *field)
{
	return depends_on_context (field->parent);
}

bool
depends_on_context (MonoMethod *target)
{
	MonoGenericContext *own = mini_method_get_context (target);

	if (own != nullptr && mono_generic_context_check_used (own) != 0)
		return true;

	return depends_on_context (target->klass);
}

/*
 * This restates mini_get_basic_type_from_generic (), which answers the same
 * question for the compiled tier and is static to mini-generic-sharing.c. It is
 * shorter because shared_form () asks for SHARE_MODE_NONE, so every variable a
 * body meets under it stands for a reference.
 */
MonoType *
shared_type (MonoType *type)
{
	if (type->byref || (type->type != MONO_TYPE_VAR && type->type != MONO_TYPE_MVAR))
		return type;

	MonoType *constraint = type->data.generic_param->gshared_constraint;

	if (constraint == nullptr)
		return mono_get_object_type ();

	return m_class_get_byval_arg (mono_class_from_mono_type_internal (constraint));
}

bool
takes_rgctx_argument (MonoMethod *method)
{
	return mono_method_needs_static_rgctx_invoke (method, TRUE) != FALSE;
}

} // namespace mono::interp
