#ifndef __MONO_INTERP_INTERP_STACKVAL_HPP__
#define __MONO_INTERP_INTERP_STACKVAL_HPP__

/**
 * \file
 * \brief Moving a value between the interpreter stack and memory.
 */

#include "internals.hpp"

#include <mono/metadata/class-internals.h>
#include <mono/metadata/object-internals.h>
#include <mono/mini/mini.h>
#include <cstring>

namespace mono::interp {

/*
 * Moving a value between the interpreter stack and memory laid out the way the
 * runtime lays out a field, an array element or a pinvoke argument. pinvoke
 * picks the native layout of a value type over the managed one.
 *
 * Both return the size the value takes on the interpreter stack, which is a
 * whole number of slots and so is not the size in memory.
 */

inline int
stackval_from_data (MonoType *type, stackval *result, const void *data, gboolean pinvoke)
{
	type = mini_native_type_replace_type (type);
	if (type->byref) {
		result->data.p = *(gpointer *) data;
		return MINT_STACK_SLOT_SIZE;
	}
	switch (type->type) {
	case MONO_TYPE_VOID:
		return 0;
	case MONO_TYPE_I1:
		result->data.i = *(gint8 *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_U1:
	case MONO_TYPE_BOOLEAN:
		result->data.i = *(guint8 *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_I2:
		result->data.i = *(gint16 *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_U2:
	case MONO_TYPE_CHAR:
		result->data.i = *(guint16 *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_I4:
		result->data.i = *(gint32 *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_U:
	case MONO_TYPE_I:
		result->data.nati = *(mono_i *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		result->data.p = *(gpointer *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_U4:
		result->data.i = *(guint32 *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_R4:
		/* memmove handles unaligned case */
		std::memmove (&result->data.f_r4, data, sizeof (float));
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
		std::memmove (&result->data.l, data, sizeof (gint64));
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_R8:
		std::memmove (&result->data.f, data, sizeof (double));
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY:
		result->data.p = *(gpointer *) data;
		return MINT_STACK_SLOT_SIZE;
	case MONO_TYPE_TYPEDBYREF:
		/*
		 * The transform pushes a TypedReference as a value of this size, so a plain
		 * copy is what the interpreter stack expects on either side of the boundary.
		 */
		std::memcpy (result, data, sizeof (MonoTypedRef));
		return ALIGN_TO (sizeof (MonoTypedRef), MINT_STACK_SLOT_SIZE);
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (type->data.klass)) {
			return stackval_from_data (mono_class_enum_basetype_internal (type->data.klass), result,
			                           data, pinvoke);
		} else {
			int size;
			if (pinvoke)
				size = mono_class_native_size (type->data.klass, NULL);
			else
				size = mono_class_value_size (type->data.klass, NULL);
			std::memcpy (result, data, size);
			return ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
	case MONO_TYPE_GENERICINST: {
		if (mono_type_generic_inst_is_valuetype (type)) {
			MonoClass *klass = mono_class_from_mono_type_internal (type);
			int size;
			if (pinvoke)
				size = mono_class_native_size (klass, NULL);
			else
				size = mono_class_value_size (klass, NULL);
			std::memcpy (result, data, size);
			return ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
		return stackval_from_data (
			m_class_get_byval_arg (type->data.generic_class->container_class), result, data,
			pinvoke);
	}
	default:
		g_error ("got type 0x%02x", type->type);
	}
}

inline int
stackval_to_data (MonoType *type, stackval *val, void *data, gboolean pinvoke)
{
	type = mini_native_type_replace_type (type);
	if (type->byref) {
		gpointer *p = (gpointer *) data;
		*p = val->data.p;
		return MINT_STACK_SLOT_SIZE;
	}
	switch (type->type) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1: {
		guint8 *p = (guint8 *) data;
		*p = val->data.i;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_BOOLEAN: {
		guint8 *p = (guint8 *) data;
		*p = (val->data.i != 0);
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_CHAR: {
		guint16 *p = (guint16 *) data;
		*p = val->data.i;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I: {
		mono_i *p = (mono_i *) data;
		/* In theory the value used by stloc should match the local var type
	 	   but in practice it sometimes doesn't (a int32 gets dup'd and stloc'd into
		   a native int - both by csc and mcs). Not sure what to do about sign extension
		   as it is outside the spec... doing the obvious */
		*p = (mono_i) val->data.nati;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_U: {
		mono_u *p = (mono_u *) data;
		/* see above. */
		*p = (mono_u) val->data.nati;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I4:
	case MONO_TYPE_U4: {
		gint32 *p = (gint32 *) data;
		*p = val->data.i;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_I8:
	case MONO_TYPE_U8: {
		std::memmove (data, &val->data.l, sizeof (gint64));
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_R4: {
		/* memmove handles unaligned case */
		std::memmove (data, &val->data.f_r4, sizeof (float));
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_R8: {
		std::memmove (data, &val->data.f, sizeof (double));
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_STRING:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_ARRAY: {
		gpointer *p = (gpointer *) data;
		mono_gc_wbarrier_generic_store_internal (p, val->data.o);
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR: {
		gpointer *p = (gpointer *) data;
		*p = val->data.p;
		return MINT_STACK_SLOT_SIZE;
	}
	case MONO_TYPE_TYPEDBYREF:
		std::memcpy (data, val, sizeof (MonoTypedRef));
		return ALIGN_TO (sizeof (MonoTypedRef), MINT_STACK_SLOT_SIZE);
	case MONO_TYPE_VALUETYPE:
		if (m_class_is_enumtype (type->data.klass)) {
			return stackval_to_data (mono_class_enum_basetype_internal (type->data.klass), val,
			                         data, pinvoke);
		} else {
			int size;
			if (pinvoke) {
				size = mono_class_native_size (type->data.klass, NULL);
				std::memcpy (data, val, size);
			} else {
				size = mono_class_value_size (type->data.klass, NULL);
				mono_value_copy_internal (data, val, type->data.klass);
			}
			return ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
	case MONO_TYPE_GENERICINST: {
		MonoClass *container_class = type->data.generic_class->container_class;

		if (m_class_is_valuetype (container_class) && !m_class_is_enumtype (container_class)) {
			MonoClass *klass = mono_class_from_mono_type_internal (type);
			int size;
			if (pinvoke) {
				size = mono_class_native_size (klass, NULL);
				std::memcpy (data, val, size);
			} else {
				size = mono_class_value_size (klass, NULL);
				mono_value_copy_internal (data, val, klass);
			}
			return ALIGN_TO (size, MINT_STACK_SLOT_SIZE);
		}
		return stackval_to_data (m_class_get_byval_arg (type->data.generic_class->container_class),
		                         val, data, pinvoke);
	}
	default:
		g_error ("got type %x", type->type);
	}
}
#define STACK_ADD_BYTES(sp, bytes) \
	((stackval *) ((char *) (sp) + ALIGN_TO (bytes, MINT_STACK_SLOT_SIZE)))
#define STACK_SUB_BYTES(sp, bytes) \
	((stackval *) ((char *) (sp) - ALIGN_TO (bytes, MINT_STACK_SLOT_SIZE)))

} // namespace mono::interp

#endif
