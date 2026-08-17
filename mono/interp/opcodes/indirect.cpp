#include "config.h"
#include "mono/interp/runtime/stackval.hpp"

#include "mintops.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"
#include "mono/interp/interp.hpp"
#include <cstring>

namespace mono::interp {

#ifdef NO_UNALIGNED_ACCESS
#define CHECK_UNALIGNED_ACCESS 1
#else
#define CHECK_UNALIGNED_ACCESS 0
#endif

#define IMPL_LDIND(opcode, slotty, ldty, check)                         \
	MONO_INTERP_OP_IMPL (opcode)                                        \
	{                                                                   \
		gpointer ptr = LOCAL_VAR (ip[2], gpointer);                     \
		if (check)                                                      \
			NULL_CHECK (ptr);                                           \
		if (CHECK_UNALIGNED_ACCESS && sizeof (ldty) > sizeof (int)      \
		    && (size_t) ptr % sizeof (void *) != 0)                     \
			std::memcpy (&LOCAL_VAR (ip[1], char), ptr, sizeof (ldty)); \
		else                                                            \
			LOCAL_VAR (ip[1], slotty) = *static_cast<ldty *> (ptr);                  \
                                                                        \
		MONO_INTERP_OP_ADVANCE ();                                      \
		MONO_INTERP_DISPATCH ();                                        \
	}

IMPL_LDIND (MINT_LDIND_I1_CHECK, int, gint8, true);
IMPL_LDIND (MINT_LDIND_U1_CHECK, int, guint8, true);
IMPL_LDIND (MINT_LDIND_I2_CHECK, int, gint16, true);
IMPL_LDIND (MINT_LDIND_U2_CHECK, int, guint16, true);
IMPL_LDIND (MINT_LDIND_I4_CHECK, int, gint32, true);
IMPL_LDIND (MINT_LDIND_U4_CHECK, int, guint32, true);
IMPL_LDIND (MINT_LDIND_I8_CHECK, gint64, gint64, true);
IMPL_LDIND (MINT_LDIND_I, gpointer, gpointer, false);
IMPL_LDIND (MINT_LDIND_I8, gint64, gint64, false);
IMPL_LDIND (MINT_LDIND_R4_CHECK, float, gfloat, true);
IMPL_LDIND (MINT_LDIND_R8_CHECK, double, double, true);
IMPL_LDIND (MINT_LDIND_REF, gpointer, gpointer, false);
IMPL_LDIND (MINT_LDIND_REF_CHECK, gpointer, gpointer, true);

#define IMPL_STIND(opcode, type)                                                                \
	MONO_INTERP_OP_IMPL (opcode)                                                                \
	{                                                                                           \
		gpointer ptr = LOCAL_VAR (ip[1], gpointer);                                             \
		NULL_CHECK (ptr);                                                                       \
		if (opcode == MINT_STIND_REF)                                                           \
			mono_gc_wbarrier_generic_store_internal (ptr, LOCAL_VAR (ip[2], MonoObject *));     \
		else if ((opcode == MINT_STIND_I8 || opcode == MINT_STIND_R8) && CHECK_UNALIGNED_ACCESS \
		         && (size_t) ptr % sizeof (void *) != 0)                                        \
			std::memcpy (ptr, &LOCAL_VAR (ip[2], char), sizeof (type));                         \
		else                                                                                    \
			*static_cast<type *> (ptr) = LOCAL_VAR (ip[2], type);                                            \
                                                                                                \
		MONO_INTERP_OP_ADVANCE ();                                                              \
		MONO_INTERP_DISPATCH ();                                                                \
	}

IMPL_STIND (MINT_STIND_REF, gpointer);
IMPL_STIND (MINT_STIND_I1, gint8);
IMPL_STIND (MINT_STIND_I2, gint16);
IMPL_STIND (MINT_STIND_I4, gint32);
IMPL_STIND (MINT_STIND_I8, gint64);
IMPL_STIND (MINT_STIND_R4, float);
IMPL_STIND (MINT_STIND_R8, double);
IMPL_STIND (MINT_STIND_I, mono_i);

/*
 * The value-type forms below check their pointers the same way IMPL_STIND does.
 * A fault inside memcpy is not at a managed address, so the signal handler
 * cannot turn it back into a NullReferenceException and the process dies
 * instead.
 */

MONO_INTERP_OP_IMPL (MINT_LDOBJ_VT)
{
	gpointer source = LOCAL_VAR (ip[2], gpointer);
	NULL_CHECK (source);
	std::memcpy (locals + ip[1], source, ip[3]);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_STOBJ_VT)
{
	auto c = static_cast<MonoClass *> (frame->imethod->data_items[ip[3]]);
	gpointer destination = LOCAL_VAR (ip[1], gpointer);

	NULL_CHECK (destination);
	mono_value_copy_internal (destination, locals + ip[2], c);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_CPOBJ)
{
	auto c = static_cast<MonoClass *> (frame->imethod->data_items[ip[3]]);
	gpointer destination = LOCAL_VAR (ip[1], gpointer);
	gpointer source = LOCAL_VAR (ip[2], gpointer);

	g_assert (m_class_is_valuetype (c));
	/* if this assertion fails, we need to add a write barrier */
	g_assert (!MONO_TYPE_IS_REFERENCE (m_class_get_byval_arg (c)));

	NULL_CHECK (destination);
	NULL_CHECK (source);
	stackval_from_data (m_class_get_byval_arg (c), static_cast<stackval *> (destination), source, FALSE);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_CPOBJ_VT)
{
	auto c = static_cast<MonoClass *> (frame->imethod->data_items[ip[3]]);
	gpointer destination = LOCAL_VAR (ip[1], gpointer);
	gpointer source = LOCAL_VAR (ip[2], gpointer);

	NULL_CHECK (destination);
	NULL_CHECK (source);
	mono_value_copy_internal (destination, source, c);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
