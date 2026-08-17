#include "config.h"
#include "mono/interp/runtime/object.hpp"

#include "mintops.hpp"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/exception.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/object.h"
#include "mono/interp/runtime/intrins.hpp"
#include "mono/interp/interp.hpp"
#include "mono/utils/memfuncs.h"

namespace mono::interp {

MONO_INTERP_OP_IMPL (MINT_INTRINS_ENUM_HASFLAG)
{
	auto klass = (MonoClass *) frame->imethod->data_items[ip[4]];
	LOCAL_VAR (ip[1], gint32) =
		enum_hasflag ((stackval *) (locals + ip[2]), (stackval *) (locals + ip[3]), klass);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_GET_HASHCODE)
{
	LOCAL_VAR (ip[1], gint32) = mono_object_hash_internal (LOCAL_VAR (ip[2], MonoObject *));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_GET_TYPE)
{
	auto o = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (o);
	LOCAL_VAR (ip[1], MonoObject *) = (MonoObject *) o->vtable->type;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_SPAN_CTOR)
{
	gpointer ptr = LOCAL_VAR (ip[2], gpointer);
	int len = LOCAL_VAR (ip[3], gint32);
	// The constructor this stands in for goes through ThrowHelper, which names
	// no argument.
	if (G_UNLIKELY (len < 0))
		THROW_EX (mono_get_exception_argument_out_of_range (NULL), ip);

	gpointer span = locals + ip[1];
	*(gpointer *) span = ptr;
	*(gint32 *) ((gpointer *) span + 1) = len;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_BYREFERENCE_GET_VALUE)
{
	LOCAL_VAR (ip[1], gpointer) = *LOCAL_VAR (ip[2], gpointer *);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_UNSAFE_ADD_BYTE_OFFSET)
{
	LOCAL_VAR (ip[1], gpointer) = LOCAL_VAR (ip[2], guint8 *) + LOCAL_VAR (ip[3], mono_u);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_UNSAFE_BYTE_OFFSET)
{
	LOCAL_VAR (ip[1], mono_u) = LOCAL_VAR (ip[3], guint8 *) - LOCAL_VAR (ip[2], guint8 *);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_RUNTIMEHELPERS_OBJECT_HAS_COMPONENT_SIZE)
{
	auto obj = LOCAL_VAR (ip[2], MonoObject *);
	LOCAL_VAR (ip[1], gint32) = (obj->vtable->flags & MONO_VT_FLAG_ARRAY_OR_STRING) != 0;

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_CLEAR_WITH_REFERENCES)
{
	gpointer p = LOCAL_VAR (ip[1], gpointer);
	size_t size = LOCAL_VAR (ip[2], mono_u) * sizeof (gpointer);
	mono_gc_bzero_aligned (p, size);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_MARVIN_BLOCK)
{
	marvin_block (LOCAL_VAR (ip[1], guint32 *), LOCAL_VAR (ip[2], guint32 *));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_ASCII_CHARS_TO_UPPERCASE)
{
	LOCAL_VAR (ip[1], gint32) =
		ascii_chars_to_uppercase (LOCAL_VAR (ip[2], guint32));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_MEMORYMARSHAL_GETARRAYDATAREF)
{
	auto o = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (o);
	LOCAL_VAR (ip[1], gpointer) = (guint8 *) o + MONO_STRUCT_OFFSET (MonoArray, vector);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_ORDINAL_IGNORE_CASE_ASCII)
{
	LOCAL_VAR (ip[1], gint32) = ordinal_ignore_case_ascii (
		LOCAL_VAR (ip[2], guint32), LOCAL_VAR (ip[3], guint32));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_64ORDINAL_IGNORE_CASE_ASCII)
{
	LOCAL_VAR (ip[1], gint32) = ordinal_ignore_case_ascii (
		LOCAL_VAR (ip[2], guint64), LOCAL_VAR (ip[3], guint64));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_U32_TO_DECSTR)
{
	auto cache_addr = (MonoArray **) frame->imethod->data_items[ip[3]];
	auto string_vtable = (MonoVTable *) frame->imethod->data_items[ip[4]];
	LOCAL_VAR (ip[1], MonoObject *) = (MonoObject *) u32_to_decstr (
		LOCAL_VAR (ip[2], guint32), *cache_addr, string_vtable);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_INTRINS_WIDEN_ASCII_TO_UTF16)
{
	LOCAL_VAR (ip[1], mono_u) = widen_ascii_to_utf16 (
		LOCAL_VAR (ip[2], guint8 *), LOCAL_VAR (ip[3], mono_unichar2 *), LOCAL_VAR (ip[4], mono_u));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
