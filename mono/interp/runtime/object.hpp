#ifndef __MONO_INTERP_INTERP_OBJECT_HPP__
#define __MONO_INTERP_INTERP_OBJECT_HPP__

/**
 * \file
 * \brief Questions the opcodes ask about an object or an enum value.
 */

#include "internals.hpp"
#include "stackval.hpp"

#include <mono/metadata/class-internals.h>
#include <mono/metadata/object-internals.h>

namespace mono::interp {

namespace {

inline bool
isinst (MonoObject *object, MonoClass *klass, MonoError *error)
{
	MonoClass *obj_class = mono_object_class (object);

	// mono_class_is_assignable_from_checked can't handle remoting casts
	if (G_UNLIKELY (mono_class_is_transparent_proxy (obj_class)))
		return mono_object_isinst_checked (object, klass, error);

	gboolean isinst = false;
	mono_class_is_assignable_from_checked (klass, obj_class, &isinst, error);
	return isinst;
}

} // namespace
inline gint32
enum_hasflag (stackval *sp1, stackval *sp2, MonoClass *klass)
{
	guint64 a_val = 0, b_val = 0;

	stackval_to_data (m_class_get_byval_arg (klass), sp1, &a_val, FALSE);
	stackval_to_data (m_class_get_byval_arg (klass), sp2, &b_val, FALSE);
	return (a_val & b_val) == b_val;
}

} // namespace mono::interp

#endif
