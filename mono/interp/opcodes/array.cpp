#include "config.h"
#include "mono/interp/runtime/object.hpp"

#include "mintops.hpp"
#include "mono/metadata/class-inlines.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/class.h"
#include "mono/metadata/exception.h"
#include "mono/metadata/object-forward.h"
#include "mono/metadata/object-internals.h"
#include "mono/interp/interp.hpp"
#include "mono/interp/runtime/array.hpp"
#include "mono/utils/mono-error-internals.h"

namespace mono::interp {

static MonoObject *
ves_array_create (MonoDomain *domain, MonoClass *klass, int param_count, stackval *values,
                  MonoError *error)
{
	int rank = m_class_get_rank (klass);
	uintptr_t *lengths = g_newa (uintptr_t, rank * 2);
	intptr_t *lower_bounds = NULL;
	if (2 * rank == param_count) {
		for (int l = 0; l < 2; ++l) {
			int src = l;
			int dst = l * rank;
			for (int r = 0; r < rank; ++r, src += 2, ++dst) {
				lengths[dst] = values[src].data.i;
			}
		}
		/* lower bounds are first. */
		lower_bounds = reinterpret_cast<intptr_t *> (lengths);
		lengths += rank;
	} else {
		/* Only lengths provided. */
		for (int i = 0; i < param_count; ++i) {
			lengths[i] = values[i].data.i;
		}
	}
	return reinterpret_cast<MonoObject *> (
		mono_array_new_full_checked (domain, klass, lengths, lower_bounds, error));
}

static MonoException *
ves_array_element_address (InterpFrame *frame, MonoClass *required_type, MonoArray *ao,
                           stackval *sp, gboolean needs_typecheck)
{
	MonoClass *ac = (reinterpret_cast<MonoObject *> (ao))->vtable->klass;

	g_assert (m_class_get_rank (ac) >= 1);

	gint32 pos = ves_array_calculate_index (ao, sp, TRUE);
	if (G_UNLIKELY (pos == -1))
		return mono_get_exception_index_out_of_range ();

	if (G_UNLIKELY (
			needs_typecheck
			&& !mono_class_is_assignable_from_internal (
				m_class_get_element_class (mono_object_class (reinterpret_cast<MonoObject *> (ao))),
				required_type)))
		return mono_get_exception_array_type_mismatch ();
	gint32 esize = mono_array_element_size (ac);
	sp[-1].data.p = mono_array_addr_with_size_fast (ao, esize, pos);
	return NULL;
}

#define IMPL_LDELEM(opcode, dtype, etype)                                       \
	MONO_INTERP_OP_IMPL (opcode)                                                \
	{                                                                           \
		MonoArray *array = LOCAL_VAR (ip[2], MonoArray *);                      \
		NULL_CHECK (array);                                                     \
		gint32 index = LOCAL_VAR (ip[3], gint32);                               \
		if (G_UNLIKELY ((guint32) index >= mono_array_length_internal (array))) \
			THROW_EX (mono_get_exception_index_out_of_range (), ip);            \
		LOCAL_VAR (ip[1], dtype) = mono_array_get_fast (array, etype, index);   \
                                                                                \
		MONO_INTERP_OP_ADVANCE ();                                              \
		MONO_INTERP_DISPATCH ();                                                \
	}

IMPL_LDELEM (MINT_LDELEM_I1, gint32, gint8);
IMPL_LDELEM (MINT_LDELEM_U1, gint32, guint8);
IMPL_LDELEM (MINT_LDELEM_I2, gint32, gint16);
IMPL_LDELEM (MINT_LDELEM_U2, gint32, guint16);
IMPL_LDELEM (MINT_LDELEM_I4, gint32, gint32);
IMPL_LDELEM (MINT_LDELEM_U4, gint32, guint32);
IMPL_LDELEM (MINT_LDELEM_I8, gint64, guint64);
IMPL_LDELEM (MINT_LDELEM_R4, float, float);
IMPL_LDELEM (MINT_LDELEM_R8, double, double);
IMPL_LDELEM (MINT_LDELEM_I, mono_u, mono_i);
IMPL_LDELEM (MINT_LDELEM_REF, gpointer, gpointer);

MONO_INTERP_OP_IMPL (MINT_LDELEM_VT)
{
	MonoArray *array = LOCAL_VAR (ip[2], MonoArray *);
	NULL_CHECK (array);
	gint32 index = LOCAL_VAR (ip[3], gint32);
	if (G_UNLIKELY ((guint32) index >= mono_array_length_internal (array)))
		THROW_EX (mono_get_exception_index_out_of_range (), ip);

	guint16 size = ip[4];
	char *src = mono_array_addr_with_size_fast (array, size, index);
	std::memcpy (&LOCAL_VAR (ip[1], char), src, size);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

#define IMPL_STELEM(opcode, dtype, etype)                                       \
	MONO_INTERP_OP_IMPL (opcode)                                                \
	{                                                                           \
		auto array = LOCAL_VAR (ip[1], MonoArray *);                            \
		NULL_CHECK (array);                                                     \
		auto index = LOCAL_VAR (ip[2], gint32);                                 \
		if (G_UNLIKELY ((guint32) index >= mono_array_length_internal (array))) \
			THROW_EX (mono_get_exception_index_out_of_range (), ip);            \
		mono_array_set_fast (array, etype, index, LOCAL_VAR (ip[3], dtype));    \
                                                                                \
		MONO_INTERP_OP_ADVANCE ();                                              \
		MONO_INTERP_DISPATCH ();                                                \
	}

IMPL_STELEM (MINT_STELEM_I1, gint32, gint8);
IMPL_STELEM (MINT_STELEM_U1, gint32, guint8);
IMPL_STELEM (MINT_STELEM_I2, gint32, gint16);
IMPL_STELEM (MINT_STELEM_U2, gint32, guint16);
IMPL_STELEM (MINT_STELEM_I4, gint32, gint32);
IMPL_STELEM (MINT_STELEM_I8, gint64, gint64);
IMPL_STELEM (MINT_STELEM_I, mono_u, mono_i);
IMPL_STELEM (MINT_STELEM_R4, float, float);
IMPL_STELEM (MINT_STELEM_R8, double, double);

MONO_INTERP_OP_IMPL (MINT_STELEM_REF)
{
	auto array = LOCAL_VAR (ip[1], MonoArray *);
	NULL_CHECK (array);
	auto index = LOCAL_VAR (ip[2], gint32);
	if (G_UNLIKELY ((guint32) index >= mono_array_length_internal (array)))
		THROW_EX (mono_get_exception_index_out_of_range (), ip);
	auto ref = LOCAL_VAR (ip[3], MonoObject *);
	if (ref) {
		ERROR_DECL (error);
		bool is_inst = isinst (ref, m_class_get_element_class (mono_object_class (array)), error);
		if (G_UNLIKELY (!is_ok (error)))
			THROW_EX (mono_error_convert_to_exception (error), ip);
		if (G_UNLIKELY (!is_inst))
			THROW_EX (mono_get_exception_array_type_mismatch (), ip);
	}

	mono_array_setref_fast (array, index, ref);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_STELEM_VT)
{
	auto array = LOCAL_VAR (ip[1], MonoArray *);
	NULL_CHECK (array);
	auto index = LOCAL_VAR (ip[2], gint32);
	if ((guint32) index >= mono_array_length_internal (array))
		THROW_EX (mono_get_exception_index_out_of_range (), ip);

	guint16 size = ip[5];
	char *dst = mono_array_addr_with_size_fast (array, size, index);
	MonoClass *vt = static_cast<MonoClass *> (frame->imethod->data_items[ip[4]]);
	mono_value_copy_internal (dst, &LOCAL_VAR (ip[3], char), vt);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDELEMA)
{
	guint16 rank = ip[2];
	guint16 esize = ip[3];
	stackval *sp = &LOCAL_VAR (ip[1], stackval);
	MonoArray *array = reinterpret_cast<MonoArray *> (sp[0].data.o);
	NULL_CHECK (array);

	g_assert (array->bounds);
	guint32 pos = 0;
	for (size_t i = 0; i < rank; ++i) {
		gint32 idx = sp[i + 1].data.i;
		gint32 lower = array->bounds[i].lower_bound;
		guint32 len = array->bounds[i].length;
		if (G_UNLIKELY (idx < lower || (guint32) (idx - lower) >= len))
			THROW_EX (mono_get_exception_index_out_of_range (), ip);
		pos = (pos * len) + (guint32) (idx - lower);
	}

	sp[0].data.p = mono_array_addr_with_size_fast (array, esize, pos);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDELEMA1)
{
	MonoArray *array = LOCAL_VAR (ip[2], MonoArray *);
	NULL_CHECK (array);
	gint32 index = LOCAL_VAR (ip[3], gint32);

	if (G_UNLIKELY ((guint32) index >= mono_array_length_internal (array)))
		THROW_EX (mono_get_exception_index_out_of_range (), ip);

	guint16 size = ip[4];
	LOCAL_VAR (ip[1], gpointer) = mono_array_addr_with_size_fast (array, size, index);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDELEMA_TC)
{
	stackval *sp = &LOCAL_VAR (ip[1], stackval);
	MonoObject *obj = static_cast<MonoObject *> (sp[0].data.o);
	NULL_CHECK (obj);

	MonoClass *klass = static_cast<MonoClass *> (frame->imethod->data_items[ip[2]]);
	if (auto ex = ves_array_element_address (frame, klass, reinterpret_cast<MonoArray *> (obj),
	                                         sp + 1, true))
		THROW_EX (ex, ip);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDLEN)
{
	auto array = LOCAL_VAR (ip[2], MonoArray *);
	NULL_CHECK (array);
	LOCAL_VAR (ip[1], mono_u) = mono_array_length_internal (array);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

// FIXME: what's the point of this opcode? It is just a LDFLD
MONO_INTERP_OP_IMPL (MINT_LDLEN_SPAN)
{
	auto obj = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (obj);
	gsize offset_length = (gsize) (gint16) ip[3];
	LOCAL_VAR (ip[1], mono_u) =
		*reinterpret_cast<gint32 *> ((reinterpret_cast<guint8 *> (obj) + offset_length));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_NEWARR)
{
	ERROR_DECL (error);
	auto vtable = static_cast<MonoVTable *> (frame->imethod->data_items[ip[3]]);
	gint32 length = LOCAL_VAR (ip[2], gint32);
	LOCAL_VAR (ip[1], MonoObject *) =
		reinterpret_cast<MonoObject *> (mono_array_new_specific_checked (vtable, length, error));
	if (G_UNLIKELY (!is_ok (error)))
		THROW_EX (mono_error_convert_to_exception (error), ip);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

/*
 * The vtable arrives in a local rather than a data item, because a body shared
 * between reference instantiations reads it out of its generic context. The
 * element type is what differs, so no two instantiations name one array class.
 */
MONO_INTERP_OP_IMPL (MINT_NEWARR_DYN)
{
	ERROR_DECL (error);
	auto vtable = LOCAL_VAR (ip[3], MonoVTable *);
	gint32 length = LOCAL_VAR (ip[2], gint32);
	LOCAL_VAR (ip[1], MonoObject *) =
		reinterpret_cast<MonoObject *> (mono_array_new_specific_checked (vtable, length, error));
	if (G_UNLIKELY (!is_ok (error)))
		THROW_EX (mono_error_convert_to_exception (error), ip);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_NEWOBJ_ARRAY)
{
	guint16 token = ip[2];
	guint16 param_count = ip[3];
	stackval *params = &LOCAL_VAR (ip[1], stackval);
	MonoClass *klass = static_cast<MonoClass *> (frame->imethod->data_items[token]);

	ERROR_DECL (error);
	LOCAL_VAR (ip[1], MonoObject *) =
		ves_array_create (frame->imethod->domain, klass, param_count, params, error);
	if (G_UNLIKELY (!is_ok (error)))
		THROW_EX (mono_error_convert_to_exception (error), ip);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_ARRAY_RANK)
{
	MonoObject *obj = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (obj);
	LOCAL_VAR (ip[1], gint32) = m_class_get_rank (mono_object_class (obj));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_ARRAY_ELEMENT_SIZE)
{
	MonoObject *obj = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (obj);
	LOCAL_VAR (ip[1], gint32) = mono_array_element_size (mono_object_class (obj));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_ARRAY_IS_PRIMITIVE)
{
	MonoObject *obj = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (obj);
	LOCAL_VAR (ip[1], gint32) =
		m_class_is_primitive (m_class_get_element_class (mono_object_class (obj)));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_GETITEM_SPAN)
{
	guint8 *span = LOCAL_VAR (ip[2], guint8 *);
	gint32 index = LOCAL_VAR (ip[3], gint32);
	NULL_CHECK (span);

	gsize offset_length = (gsize) (gint16) ip[5];
	gint32 length = *reinterpret_cast<gint32 *> ((span + offset_length));
	if (G_UNLIKELY (index < 0 || index >= length))
		THROW_EX (mono_get_exception_index_out_of_range (), ip);

	gsize element_size = (gsize) (gint16) ip[4];
	gsize offset_pointer = (gsize) (gint16) ip[6];

	const gpointer pointer = *reinterpret_cast<gpointer *> ((span + offset_pointer));
	LOCAL_VAR (ip[1], gpointer) = static_cast<guint8 *> (pointer) + index * element_size;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
