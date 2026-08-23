/**
 * \file
 * intrinsics for variable sized int/floats
 *
 * Author:
 *   Rodrigo Kumpera (kumpera@gmail.com)
 *
 * (C) 2013 Xamarin
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */

#include <config.h>
#include <stdio.h>

#include "mini.h"
#include "glib.h"


gsize
mini_magic_type_size (MonoType *type)
{
	if (type->type == MONO_TYPE_I4 || type->type == MONO_TYPE_U4)
		return 4;
	else if (type->type == MONO_TYPE_I8 || type->type == MONO_TYPE_U8)
		return 8;
	else if (type->type == MONO_TYPE_R4 && !type->byref)
		return 4;
	else if (type->type == MONO_TYPE_R8 && !type->byref)
		return 8;
	return TARGET_SIZEOF_VOID_P;
}


static gboolean
mono_class_is_magic_assembly (MonoClass *klass)
{
	const char *aname = m_class_get_image (klass)->assembly_name;
	if (!aname)
		return FALSE;
	if (!strcmp ("Xamarin.iOS", aname))
		return TRUE;
	if (!strcmp ("Xamarin.Mac", aname))
		return TRUE;
	if (!strcmp ("Xamarin.WatchOS", aname))
		return TRUE;
	/* regression test suite */
	if (!strcmp ("builtin-types", aname))
		return TRUE;
	if (!strcmp ("mini_tests", aname))
		return TRUE;
	return FALSE;
}

/*
 * Aborts unless a magic integer is the size of the native type it stands for.
 *
 * The interpreter holds a magic type as that native type, and CEE_LDFLD reads
 * the field straight out of it. Those two views name the same bytes only while
 * the sizes agree.
 */
static void
check_magic_int_layout (MonoClass *klass)
{
	MonoType *declared = m_class_get_byval_arg (klass);
	MonoType *native = mini_native_type_replace_type (declared);
	int align;
	int declared_size = mono_type_size (declared, &align);
	int native_size = mono_type_size (native, &align);

	/* The size, rather than the field type mono_class_is_magic_float ()
	 * compares: nint declares a 64-bit field where the native type is a
	 * native int. */
	if (declared_size != native_size)
		g_error ("Assembly used for native types '%s' doesn't match this runtime, %s is %d bytes where %s is %d.\n",
		         m_class_get_image (klass)->name, m_class_get_name (klass), declared_size,
		         mono_type_full_name (native), native_size);
}

gboolean
mono_class_is_magic_int (MonoClass *klass)
{
	static MonoClass *magic_nint_class;
	static MonoClass *magic_nuint_class;

	if (klass == magic_nint_class)
		return TRUE;

	if (klass == magic_nuint_class)
		return TRUE;

	if (magic_nint_class && magic_nuint_class)
		return FALSE;

	if (!mono_class_is_magic_assembly (klass))
		return FALSE;

	if (strcmp ("System", m_class_get_name_space (klass)) != 0)
		return FALSE;

	if (strcmp ("nint", m_class_get_name (klass)) == 0) {
		magic_nint_class = klass;
		check_magic_int_layout (klass);
		return TRUE;
	}

	if (strcmp ("nuint", m_class_get_name (klass)) == 0){
		magic_nuint_class = klass;
		check_magic_int_layout (klass);
		return TRUE;
	}
	return FALSE;
}

gboolean
mono_class_is_magic_float (MonoClass *klass)
{
	static MonoClass *magic_nfloat_class;

	if (klass == magic_nfloat_class)
		return TRUE;

	if (magic_nfloat_class)
		return FALSE;

	if (!mono_class_is_magic_assembly (klass))
		return FALSE;

	if (strcmp ("System", m_class_get_name_space (klass)) != 0)
		return FALSE;

	if (strcmp ("nfloat", m_class_get_name (klass)) == 0) {
		magic_nfloat_class = klass;

		/* Assert that we are using the matching assembly */
		MonoClassField *value_field = mono_class_get_field_from_name_full (klass, "v", NULL);
		g_assert (value_field);
		MonoType *t = mono_field_get_type_internal (value_field);
		MonoType *native = mini_native_type_replace_type (m_class_get_byval_arg (klass));
		if (t->type != native->type)
			g_error ("Assembly used for native types '%s' doesn't match this runtime, %s is mapped to %s, expecting %s.\n", m_class_get_image (klass)->name, m_class_get_name (klass), mono_type_full_name (t), mono_type_full_name (native));
		return TRUE;
	}
	return FALSE;
}

gboolean
mini_magic_is_int_type (MonoType *t)
{
	if (t->type != MONO_TYPE_I && t->type != MONO_TYPE_I4 && t->type != MONO_TYPE_I8 && t->type != MONO_TYPE_U4 && t->type != MONO_TYPE_U8 && !mono_class_is_magic_int (mono_class_from_mono_type_internal (t)))
		return FALSE;
	return TRUE;
}

gboolean
mini_magic_is_float_type (MonoType *t)
{
	if (t->type != MONO_TYPE_R4 && t->type != MONO_TYPE_R8 && !mono_class_is_magic_float (mono_class_from_mono_type_internal (t)))
		return FALSE;
	return TRUE;
}

MonoType*
mini_native_type_replace_type (MonoType *type)
{
	MonoClass *klass;

	if (type->type != MONO_TYPE_VALUETYPE)
		return type;
	klass = type->data.klass;

	if (mono_class_is_magic_int (klass))
		return type->byref ? m_class_get_this_arg (mono_defaults.int_class) : mono_get_int_type ();
	if (mono_class_is_magic_float (klass))
#if TARGET_SIZEOF_VOID_P == 8
		return type->byref ? m_class_get_this_arg (mono_defaults.double_class) : m_class_get_byval_arg (mono_defaults.double_class);
#else
		return type->byref ? m_class_get_this_arg (mono_defaults.single_class) : m_class_get_byval_arg (mono_defaults.single_class);
#endif
	return type;
}
