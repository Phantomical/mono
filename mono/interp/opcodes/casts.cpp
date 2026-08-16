#include "mintops.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/class.h"
#include "mono/metadata/exception.h"
#include "mono/metadata/gc-internals.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/object.h"
#include "mono/interp/interp-internals.hpp"
#include "mono/interp/interp.hpp"
#include "mono/utils/mono-error-internals.h"

namespace mono::interp {

/*
 * A box allocates, so the new object is only reachable through tmp_handle until it
 * reaches a local. Everything filling it in runs between the two assignments.
 */
MONO_INTERP_OP_IMPL (MINT_BOX)
{
	auto vtable = (MonoVTable *) frame->imethod->data_items[ip[3]];

	MonoObject *o = mono_gc_alloc_obj (vtable, m_class_get_instance_size (vtable->klass));
	MONO_HANDLE_ASSIGN_RAW (tmp_handle, o);
	stackval_to_data (m_class_get_byval_arg (vtable->klass), (stackval *) (locals + ip[2]),
	                  mono_object_get_data (o), FALSE);
	MONO_HANDLE_ASSIGN_RAW (tmp_handle, NULL);

	LOCAL_VAR (ip[1], MonoObject *) = o;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_BOX_VT)
{
	auto vtable = (MonoVTable *) frame->imethod->data_items[ip[3]];
	MonoClass *c = vtable->klass;

	MonoObject *o = mono_gc_alloc_obj (vtable, m_class_get_instance_size (c));
	MONO_HANDLE_ASSIGN_RAW (tmp_handle, o);
	mono_value_copy_internal (mono_object_get_data (o), locals + ip[2], c);
	MONO_HANDLE_ASSIGN_RAW (tmp_handle, NULL);

	LOCAL_VAR (ip[1], MonoObject *) = o;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_BOX_PTR)
{
	auto vtable = (MonoVTable *) frame->imethod->data_items[ip[3]];
	MonoClass *c = vtable->klass;

	MonoObject *o = mono_gc_alloc_obj (vtable, m_class_get_instance_size (c));
	MONO_HANDLE_ASSIGN_RAW (tmp_handle, o);
	mono_value_copy_internal (mono_object_get_data (o), LOCAL_VAR (ip[2], gpointer), c);
	MONO_HANDLE_ASSIGN_RAW (tmp_handle, NULL);

	LOCAL_VAR (ip[1], MonoObject *) = o;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_BOX_NULLABLE_PTR)
{
	auto c = (MonoClass *) frame->imethod->data_items[ip[3]];

	error_init_reuse (error);
	LOCAL_VAR (ip[1], MonoObject *) = mono_nullable_box (LOCAL_VAR (ip[2], gpointer), c, error);
	mono_interp_error_cleanup (error); // FIXME: don't swallow the error

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_UNBOX)
{
	auto o = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (o);
	auto c = (MonoClass *) frame->imethod->data_items[ip[3]];

	if (!(m_class_get_rank (o->vtable->klass) == 0
	      && m_class_get_element_class (o->vtable->klass) == m_class_get_element_class (c)))
		THROW_EX (mono_get_exception_invalid_cast (), ip);

	LOCAL_VAR (ip[1], gpointer) = mono_object_unbox_internal (o);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

/*
 * castclass and isinst ask the same question and differ only in what a no means:
 * castclass throws where isinst writes null. The three pairs below differ in how
 * the question is answered, which is what the transform picked the opcode for.
 */
#define IMPL_CAST(opcode, throwing, answer)                              \
	MONO_INTERP_OP_IMPL (opcode)                                         \
	{                                                                    \
		auto o = LOCAL_VAR (ip[2], MonoObject *);                        \
                                                                         \
		if (o) {                                                         \
			auto c = (MonoClass *) frame->imethod->data_items[ip[3]];    \
                                                                         \
			if (!(answer)) {                                             \
				if (throwing)                                            \
					THROW_EX (mono_get_exception_invalid_cast (), ip);   \
				else                                                     \
					LOCAL_VAR (ip[1], MonoObject *) = nullptr;           \
			} else {                                                     \
				LOCAL_VAR (ip[1], MonoObject *) = o;                     \
			}                                                            \
		} else {                                                         \
			LOCAL_VAR (ip[1], MonoObject *) = nullptr;                   \
		}                                                                \
                                                                         \
		MONO_INTERP_OP_ADVANCE ();                                       \
		MONO_INTERP_DISPATCH ();                                         \
	}

// FIXME: do not swallow the error
#define CAST_GENERAL   interp_isinst (o, c)
#define CAST_COMMON    mono_class_has_parent_fast (o->vtable->klass, c)
#define CAST_INTERFACE interp_isinst_interface (o, c)

static gboolean
interp_isinst (MonoObject *o, MonoClass *c)
{
	ERROR_DECL (error);
	gboolean res = isinst (o, c, error);

	mono_error_cleanup (error); // FIXME: don't swallow the error
	return res;
}

static gboolean
interp_isinst_interface (MonoObject *o, MonoClass *c)
{
	if (MONO_VTABLE_IMPLEMENTS_INTERFACE (o->vtable, m_class_get_interface_id (c)))
		return TRUE;

	// An array special interface is implemented by a variance rule rather than by
	// the vtable, and a proxy answers for the class it stands in for.
	if (m_class_is_array_special_interface (c) || mono_object_is_transparent_proxy (o))
		return interp_isinst (o, c);

	return FALSE;
}

IMPL_CAST (MINT_CASTCLASS, true, CAST_GENERAL);
IMPL_CAST (MINT_ISINST, false, CAST_GENERAL);
IMPL_CAST (MINT_CASTCLASS_COMMON, true, CAST_COMMON);
IMPL_CAST (MINT_ISINST_COMMON, false, CAST_COMMON);
IMPL_CAST (MINT_CASTCLASS_INTERFACE, true, CAST_INTERFACE);
IMPL_CAST (MINT_ISINST_INTERFACE, false, CAST_INTERFACE);

MONO_INTERP_OP_IMPL (MINT_MKREFANY)
{
	auto c = (MonoClass *) frame->imethod->data_items[ip[3]];
	gpointer addr = LOCAL_VAR (ip[2], gpointer);

	auto tref = (MonoTypedRef *) (locals + ip[1]);
	tref->klass = c;
	tref->type = m_class_get_byval_arg (c);
	tref->value = addr;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_REFANYTYPE)
{
	auto tref = (MonoTypedRef *) (locals + ip[2]);

	LOCAL_VAR (ip[1], gpointer) = tref->type;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_REFANYVAL)
{
	auto tref = (MonoTypedRef *) (locals + ip[2]);
	auto c = (MonoClass *) frame->imethod->data_items[ip[3]];

	if (c != tref->klass)
		THROW_EX (mono_get_exception_invalid_cast (), ip);

	LOCAL_VAR (ip[1], gpointer) = tref->value;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
