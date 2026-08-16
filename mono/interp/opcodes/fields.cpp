#include "mintops.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/class.h"
#include "mono/metadata/object-forward.h"
#include "mono/metadata/object-internals.h"
#include "mono/metadata/threads-types.h"
#include "mono/interp/interp-internals.hpp"
#include "mono/interp/interp.hpp"
#include "mono/utils/mono-compiler.h"
#include "mono/utils/mono-error-internals.h"
#include <cstring>

namespace mono::interp {

/*
 * Field access. The families differ in where the field lives:
 *
 *   MINT_LDFLD_VT_*   a field of a value type in a local, at ip[2] + ip[3]
 *   MINT_LDFLD_*      a field of the object in ip[2], at offset ip[3]
 *   MINT_LDSFLD_*     a static field, whose address the transform already put
 *                     in data_items[ip[3]]
 *
 * with MINT_STFLD_* and MINT_STSFLD_* as the matching stores. The suffix gives
 * the width of the field in memory. A field narrower than int32 loads into an
 * int32 destination, because ECMA-335 Partition III has no narrower stack type.
 *
 * An _UNALIGNED variant copies byte by byte instead. The transform emits one
 * where it cannot prove the field is aligned, so the two forms are separate
 * opcodes rather than a test made here.
 */

#define IMPL_LDFLD_VT(opcode, datatype, fieldtype)                          \
	MONO_INTERP_OP_IMPL (opcode)                                            \
	{                                                                       \
		LOCAL_VAR (ip[1], datatype) = LOCAL_VAR (ip[2] + ip[3], fieldtype); \
                                                                            \
		MONO_INTERP_OP_ADVANCE ();                                          \
		MONO_INTERP_DISPATCH ();                                            \
	}

IMPL_LDFLD_VT (MINT_LDFLD_VT_I1, gint32, gint8);
IMPL_LDFLD_VT (MINT_LDFLD_VT_U1, gint32, guint8);
IMPL_LDFLD_VT (MINT_LDFLD_VT_I2, gint32, gint16);
IMPL_LDFLD_VT (MINT_LDFLD_VT_U2, gint32, guint16);
IMPL_LDFLD_VT (MINT_LDFLD_VT_I4, gint32, gint32);
IMPL_LDFLD_VT (MINT_LDFLD_VT_I8, gint64, gint64);
IMPL_LDFLD_VT (MINT_LDFLD_VT_R4, float, float);
IMPL_LDFLD_VT (MINT_LDFLD_VT_R8, double, double);
IMPL_LDFLD_VT (MINT_LDFLD_VT_O, gpointer, gpointer);

#define IMPL_LDFLD_VT_UNALIGNED(opcode, fieldtype)                                \
	MONO_INTERP_OP_IMPL (opcode)                                                  \
	{                                                                             \
		std::memcpy (locals + ip[1], locals + ip[2] + ip[3], sizeof (fieldtype)); \
                                                                                  \
		MONO_INTERP_OP_ADVANCE ();                                                \
		MONO_INTERP_DISPATCH ();                                                  \
	}

IMPL_LDFLD_VT_UNALIGNED (MINT_LDFLD_VT_I8_UNALIGNED, gint64);
IMPL_LDFLD_VT_UNALIGNED (MINT_LDFLD_VT_R8_UNALIGNED, double);

// Source and destination are both interpreter locals, so they can overlap.
MONO_INTERP_OP_IMPL (MINT_LDFLD_VT_VT)
{
	std::memmove (locals + ip[1], locals + ip[2] + ip[3], ip[4]);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

#define IMPL_LDFLD(opcode, datatype, fieldtype)                            \
	MONO_INTERP_OP_IMPL (opcode)                                           \
	{                                                                      \
		auto o = LOCAL_VAR (ip[2], MonoObject *);                          \
		NULL_CHECK (o);                                                    \
		LOCAL_VAR (ip[1], datatype) = *(fieldtype *) ((char *) o + ip[3]); \
                                                                           \
		MONO_INTERP_OP_ADVANCE ();                                         \
		MONO_INTERP_DISPATCH ();                                           \
	}

IMPL_LDFLD (MINT_LDFLD_I1, gint32, gint8);
IMPL_LDFLD (MINT_LDFLD_U1, gint32, guint8);
IMPL_LDFLD (MINT_LDFLD_I2, gint32, gint16);
IMPL_LDFLD (MINT_LDFLD_U2, gint32, guint16);
IMPL_LDFLD (MINT_LDFLD_I4, gint32, gint32);
IMPL_LDFLD (MINT_LDFLD_I8, gint64, gint64);
IMPL_LDFLD (MINT_LDFLD_R4, float, float);
IMPL_LDFLD (MINT_LDFLD_R8, double, double);
IMPL_LDFLD (MINT_LDFLD_O, gpointer, gpointer);

#define IMPL_LDFLD_UNALIGNED(opcode, fieldtype)                               \
	MONO_INTERP_OP_IMPL (opcode)                                              \
	{                                                                         \
		auto o = LOCAL_VAR (ip[2], MonoObject *);                             \
		NULL_CHECK (o);                                                       \
		std::memcpy (locals + ip[1], (char *) o + ip[3], sizeof (fieldtype)); \
                                                                              \
		MONO_INTERP_OP_ADVANCE ();                                            \
		MONO_INTERP_DISPATCH ();                                              \
	}

IMPL_LDFLD_UNALIGNED (MINT_LDFLD_I8_UNALIGNED, gint64);
IMPL_LDFLD_UNALIGNED (MINT_LDFLD_R8_UNALIGNED, double);

MONO_INTERP_OP_IMPL (MINT_LDFLD_VT)
{
	auto o = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (o);
	std::memcpy (locals + ip[1], (char *) o + ip[3], ip[4]);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

#define IMPL_STFLD(opcode, datatype, fieldtype)                            \
	MONO_INTERP_OP_IMPL (opcode)                                           \
	{                                                                      \
		auto o = LOCAL_VAR (ip[1], MonoObject *);                          \
		NULL_CHECK (o);                                                    \
		*(fieldtype *) ((char *) o + ip[3]) = LOCAL_VAR (ip[2], datatype); \
                                                                           \
		MONO_INTERP_OP_ADVANCE ();                                         \
		MONO_INTERP_DISPATCH ();                                           \
	}

IMPL_STFLD (MINT_STFLD_I1, gint32, gint8);
IMPL_STFLD (MINT_STFLD_U1, gint32, guint8);
IMPL_STFLD (MINT_STFLD_I2, gint32, gint16);
IMPL_STFLD (MINT_STFLD_U2, gint32, guint16);
IMPL_STFLD (MINT_STFLD_I4, gint32, gint32);
IMPL_STFLD (MINT_STFLD_I8, gint64, gint64);
IMPL_STFLD (MINT_STFLD_R4, float, float);
IMPL_STFLD (MINT_STFLD_R8, double, double);

#define IMPL_STFLD_UNALIGNED(opcode, fieldtype)                               \
	MONO_INTERP_OP_IMPL (opcode)                                              \
	{                                                                         \
		auto o = LOCAL_VAR (ip[1], MonoObject *);                             \
		NULL_CHECK (o);                                                       \
		std::memcpy ((char *) o + ip[3], locals + ip[2], sizeof (fieldtype)); \
                                                                              \
		MONO_INTERP_OP_ADVANCE ();                                            \
		MONO_INTERP_DISPATCH ();                                              \
	}

IMPL_STFLD_UNALIGNED (MINT_STFLD_I8_UNALIGNED, gint64);
IMPL_STFLD_UNALIGNED (MINT_STFLD_R8_UNALIGNED, double);

MONO_INTERP_OP_IMPL (MINT_STFLD_O)
{
	auto o = LOCAL_VAR (ip[1], MonoObject *);
	NULL_CHECK (o);
	mono_gc_wbarrier_set_field_internal (o, (char *) o + ip[3], LOCAL_VAR (ip[2], MonoObject *));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

// The value type holds no reference, so the copy needs no barrier.
MONO_INTERP_OP_IMPL (MINT_STFLD_VT_NOREF)
{
	auto o = LOCAL_VAR (ip[1], MonoObject *);
	NULL_CHECK (o);
	std::memcpy ((char *) o + ip[3], locals + ip[2], ip[4]);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_STFLD_VT)
{
	auto o = LOCAL_VAR (ip[1], MonoObject *);
	NULL_CHECK (o);
	auto klass = (MonoClass *) frame->imethod->data_items[ip[4]];
	mono_value_copy_internal ((char *) o + ip[3], locals + ip[2], klass);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

#define IMPL_LDSFLD(opcode, datatype, fieldtype)                                          \
	MONO_INTERP_OP_IMPL (opcode)                                                          \
	{                                                                                     \
		auto vtable = (MonoVTable *) frame->imethod->data_items[ip[2]];                   \
		INIT_VTABLE (vtable);                                                             \
		LOCAL_VAR (ip[1], datatype) = *(fieldtype *) (frame->imethod->data_items[ip[3]]); \
                                                                                          \
		MONO_INTERP_OP_ADVANCE ();                                                        \
		MONO_INTERP_DISPATCH ();                                                          \
	}

IMPL_LDSFLD (MINT_LDSFLD_I1, gint32, gint8);
IMPL_LDSFLD (MINT_LDSFLD_U1, gint32, guint8);
IMPL_LDSFLD (MINT_LDSFLD_I2, gint32, gint16);
IMPL_LDSFLD (MINT_LDSFLD_U2, gint32, guint16);
IMPL_LDSFLD (MINT_LDSFLD_I4, gint32, gint32);
IMPL_LDSFLD (MINT_LDSFLD_I8, gint64, gint64);
IMPL_LDSFLD (MINT_LDSFLD_R4, float, float);
IMPL_LDSFLD (MINT_LDSFLD_R8, double, double);
IMPL_LDSFLD (MINT_LDSFLD_O, gpointer, gpointer);

MONO_INTERP_OP_IMPL (MINT_LDSFLD_VT)
{
	auto vtable = (MonoVTable *) frame->imethod->data_items[ip[2]];
	INIT_VTABLE (vtable);

	gpointer addr = frame->imethod->data_items[ip[3]];
	std::memcpy (locals + ip[1], addr, ip[4]);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

#define IMPL_STSFLD(opcode, datatype, fieldtype)                                          \
	MONO_INTERP_OP_IMPL (opcode)                                                          \
	{                                                                                     \
		auto vtable = (MonoVTable *) frame->imethod->data_items[ip[2]];                   \
		INIT_VTABLE (vtable);                                                             \
		*(fieldtype *) (frame->imethod->data_items[ip[3]]) = LOCAL_VAR (ip[1], datatype); \
                                                                                          \
		MONO_INTERP_OP_ADVANCE ();                                                        \
		MONO_INTERP_DISPATCH ();                                                          \
	}

IMPL_STSFLD (MINT_STSFLD_I1, gint32, gint8);
IMPL_STSFLD (MINT_STSFLD_U1, gint32, guint8);
IMPL_STSFLD (MINT_STSFLD_I2, gint32, gint16);
IMPL_STSFLD (MINT_STSFLD_U2, gint32, guint16);
IMPL_STSFLD (MINT_STSFLD_I4, gint32, gint32);
IMPL_STSFLD (MINT_STSFLD_I8, gint64, gint64);
IMPL_STSFLD (MINT_STSFLD_R4, float, float);
IMPL_STSFLD (MINT_STSFLD_R8, double, double);

MONO_INTERP_OP_IMPL (MINT_STSFLD_O)
{
	auto vtable = (MonoVTable *) frame->imethod->data_items[ip[2]];
	INIT_VTABLE (vtable);
	mono_gc_wbarrier_generic_store_internal (frame->imethod->data_items[ip[3]],
	                                         LOCAL_VAR (ip[1], MonoObject *));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_STSFLD_VT)
{
	auto vtable = (MonoVTable *) frame->imethod->data_items[ip[2]];
	INIT_VTABLE (vtable);

	gpointer addr = frame->imethod->data_items[ip[3]];
	std::memcpy (addr, locals + ip[1], ip[4]);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

/*
 * The field addresses. A ldflda site gives the address of a field, which the IL
 * then reads or writes through a ldind or stind.
 */

MONO_INTERP_OP_IMPL (MINT_LDFLDA)
{
	auto o = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (o);
	LOCAL_VAR (ip[1], gpointer) = (char *) o + ip[3];

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

// The transform emits this where it knows the receiver cannot be null.
MONO_INTERP_OP_IMPL (MINT_LDFLDA_UNSAFE)
{
	LOCAL_VAR (ip[1], gpointer) = (char *) LOCAL_VAR (ip[2], gpointer) + ip[3];

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDSFLDA)
{
	auto vtable = (MonoVTable *) frame->imethod->data_items[ip[2]];
	INIT_VTABLE (vtable);
	LOCAL_VAR (ip[1], gpointer) = frame->imethod->data_items[ip[3]];

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

/*
 * The thread statics. Instead of one address the transform can bake in, the
 * field has a packed offset: the low 6 bits pick a block out of the thread's
 * static_data and the rest is the offset into that block.
 */

static inline gpointer
thread_static_address (guint32 offset)
{
	MonoInternalThread *thread = mono_thread_internal_current ();
	return (char *) thread->static_data[offset & 0x3f] + (offset >> 6);
}

#define IMPL_LDTSFLD(opcode, datatype, fieldtype)                \
	MONO_INTERP_OP_IMPL (opcode)                                 \
	{                                                            \
		gpointer addr = thread_static_address (READ32 (ip + 2)); \
		LOCAL_VAR (ip[1], datatype) = *(fieldtype *) addr;       \
                                                                 \
		MONO_INTERP_OP_ADVANCE ();                               \
		MONO_INTERP_DISPATCH ();                                 \
	}

IMPL_LDTSFLD (MINT_LDTSFLD_I1, gint32, gint8);
IMPL_LDTSFLD (MINT_LDTSFLD_U1, gint32, guint8);
IMPL_LDTSFLD (MINT_LDTSFLD_I2, gint32, gint16);
IMPL_LDTSFLD (MINT_LDTSFLD_U2, gint32, guint16);
IMPL_LDTSFLD (MINT_LDTSFLD_I4, gint32, gint32);
IMPL_LDTSFLD (MINT_LDTSFLD_I8, gint64, gint64);
IMPL_LDTSFLD (MINT_LDTSFLD_R4, float, float);
IMPL_LDTSFLD (MINT_LDTSFLD_R8, double, double);
IMPL_LDTSFLD (MINT_LDTSFLD_O, gpointer, gpointer);

#define IMPL_STTSFLD(opcode, datatype, fieldtype)                \
	MONO_INTERP_OP_IMPL (opcode)                                 \
	{                                                            \
		gpointer addr = thread_static_address (READ32 (ip + 2)); \
		*(fieldtype *) addr = LOCAL_VAR (ip[1], datatype);       \
                                                                 \
		MONO_INTERP_OP_ADVANCE ();                               \
		MONO_INTERP_DISPATCH ();                                 \
	}

IMPL_STTSFLD (MINT_STTSFLD_I1, gint32, gint8);
IMPL_STTSFLD (MINT_STTSFLD_U1, gint32, guint8);
IMPL_STTSFLD (MINT_STTSFLD_I2, gint32, gint16);
IMPL_STTSFLD (MINT_STTSFLD_U2, gint32, guint16);
IMPL_STTSFLD (MINT_STTSFLD_I4, gint32, gint32);
IMPL_STTSFLD (MINT_STTSFLD_I8, gint64, gint64);
IMPL_STTSFLD (MINT_STTSFLD_R4, float, float);
IMPL_STTSFLD (MINT_STTSFLD_R8, double, double);

// A thread-static reference needs the barrier for the same reason MINT_STSFLD_O
// does.
MONO_INTERP_OP_IMPL (MINT_STTSFLD_O)
{
	gpointer addr = thread_static_address (READ32 (ip + 2));
	mono_gc_wbarrier_generic_store_internal (addr, LOCAL_VAR (ip[1], MonoObject *));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

/*
 * The special statics, which the runtime places per thread or per context. The
 * offset is again packed, but only mono_get_special_static_data () knows how to
 * read it.
 */

MONO_INTERP_OP_IMPL (MINT_LDSSFLDA)
{
	LOCAL_VAR (ip[1], gpointer) = mono_get_special_static_data (READ32 (ip + 2));

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDSSFLD)
{
	gpointer addr = mono_get_special_static_data (READ32 (ip + 3));
	auto field = (MonoClassField *) frame->imethod->data_items[ip[2]];
	stackval_from_data (field->type, &LOCAL_VAR (ip[1], stackval), addr, FALSE);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_STSSFLD)
{
	gpointer addr = mono_get_special_static_data (READ32 (ip + 3));
	auto field = (MonoClassField *) frame->imethod->data_items[ip[2]];
	stackval_to_data (field->type, &LOCAL_VAR (ip[1], stackval), addr, FALSE);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDSSFLD_VT)
{
	gpointer addr = mono_get_special_static_data (READ32 (ip + 2));
	std::memcpy (locals + ip[1], addr, ip[4]);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_STSSFLD_VT)
{
	gpointer addr = mono_get_special_static_data (READ32 (ip + 2));
	std::memcpy (addr, locals + ip[1], ip[4]);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

/*
 * The remoted fields. A transparent proxy has no field storage of its own, so
 * the access goes to the remote object instead of to an offset in this one. The
 * transform emits these where it cannot rule the proxy out.
 */

MONO_INTERP_OP_IMPL (MINT_LDRMFLD)
{
	auto o = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (o);

	auto field = (MonoClassField *) frame->imethod->data_items[ip[3]];

	gpointer addr;
#ifndef DISABLE_REMOTING
	gpointer tmp;
	if (G_UNLIKELY (mono_object_is_transparent_proxy (o))) {
		ERROR_DECL (error);
		MonoClass *klass = ((MonoTransparentProxy *) o)->remote_class->proxy_class;
		addr = mono_load_remote_field_checked (o, klass, field, &tmp, error);
		if (G_UNLIKELY (!is_ok (error)))
			THROW_EX (mono_error_convert_to_exception (error), ip);
	} else
#endif
		addr = (char *) o + field->offset;

	stackval_from_data (field->type, &LOCAL_VAR (ip[1], stackval), addr, FALSE);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDRMFLD_VT)
{
	auto o = LOCAL_VAR (ip[2], MonoObject *);
	NULL_CHECK (o);

	auto field = (MonoClassField *) frame->imethod->data_items[ip[3]];
	MonoClass *klass = mono_class_from_mono_type_internal (field->type);
	int size = mono_class_value_size (klass, NULL);

	gpointer addr;
#ifndef DISABLE_REMOTING
	gpointer tmp;
	if (G_UNLIKELY (mono_object_is_transparent_proxy (o))) {
		ERROR_DECL (error);
		klass = ((MonoTransparentProxy *) o)->remote_class->proxy_class;
		addr = mono_load_remote_field_checked (o, klass, field, &tmp, error);
		if (G_UNLIKELY (!is_ok (error)))
			THROW_EX (mono_error_convert_to_exception (error), ip);
	} else
#endif
		addr = (char *) o + field->offset;

	std::memcpy (locals + ip[1], addr, size);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_STRMFLD)
{
	auto o = LOCAL_VAR (ip[1], MonoObject *);
	NULL_CHECK (o);

	auto field = (MonoClassField *) frame->imethod->data_items[ip[3]];

#ifndef DISABLE_REMOTING
	if (G_UNLIKELY (mono_object_is_transparent_proxy (o))) {
		ERROR_DECL (error);
		MonoClass *klass = ((MonoTransparentProxy *) o)->remote_class->proxy_class;
		mono_store_remote_field_checked (o, klass, field, locals + ip[2], error);
		if (G_UNLIKELY (!is_ok (error)))
			THROW_EX (mono_error_convert_to_exception (error), ip);
	} else
#endif
		stackval_to_data (field->type, &LOCAL_VAR (ip[2], stackval), (char *) o + field->offset,
		                  FALSE);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_STRMFLD_VT)
{
	auto o = LOCAL_VAR (ip[1], MonoObject *);
	NULL_CHECK (o);

	auto field = (MonoClassField *) frame->imethod->data_items[ip[3]];
	MonoClass *klass = mono_class_from_mono_type_internal (field->type);

#ifndef DISABLE_REMOTING
	if (G_UNLIKELY (mono_object_is_transparent_proxy (o))) {
		ERROR_DECL (error);
		MonoClass *proxy_class = ((MonoTransparentProxy *) o)->remote_class->proxy_class;
		mono_store_remote_field_checked (o, proxy_class, field, locals + ip[2], error);
		if (G_UNLIKELY (!is_ok (error)))
			THROW_EX (mono_error_convert_to_exception (error), ip);
	} else
#endif
		mono_value_copy_internal ((char *) o + field->offset, locals + ip[2], klass);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

#define IMPL_MOV(opcode, dsttype, srctype)                     \
	MONO_INTERP_OP_IMPL (opcode)                               \
	{                                                          \
		LOCAL_VAR (ip[1], dsttype) = LOCAL_VAR (ip[2], srctype); \
                                                               \
		MONO_INTERP_OP_ADVANCE ();                             \
		MONO_INTERP_DISPATCH ();                               \
	}

// Loading from a local may need a sign or zero extension to 4 bytes, which is the
// smallest thing the interpreter holds a value in. Only a local whose address is
// taken needs one, since nothing can propagate that local away.
IMPL_MOV (MINT_MOV_I1, guint32, gint8);
IMPL_MOV (MINT_MOV_U1, guint32, guint8);
IMPL_MOV (MINT_MOV_I2, guint32, gint16);
IMPL_MOV (MINT_MOV_U2, guint32, guint16);

IMPL_MOV (MINT_MOV_4, guint32, guint32);
IMPL_MOV (MINT_MOV_8, guint64, guint64);

// Source and destination are both interpreter locals, so they can overlap.
MONO_INTERP_OP_IMPL (MINT_MOV_VT)
{
	std::memmove (locals + ip[1], locals + ip[2], ip[3]);

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

MONO_INTERP_OP_IMPL (MINT_LDLOCA_S)
{
	LOCAL_VAR (ip[1], gpointer) = locals + ip[2];

	MONO_INTERP_OP_ADVANCE ();
	MONO_INTERP_DISPATCH ();
}

} // namespace mono::interp
