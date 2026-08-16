#ifndef __MONO_INTERP_INTERP_ARRAY_HPP__
#define __MONO_INTERP_INTERP_ARRAY_HPP__

/**
 * \file
 * \brief Array access shared by the array opcodes and Array's runtime methods.
 */

#include "interp-internals.h"
#include "mono/metadata/class-internals.h"
#include "mono/metadata/object-internals.h"

namespace mono::interp {

inline gint32
ves_array_calculate_index (MonoArray *ao, stackval *sp, gboolean safe)
{
	MonoClass *ac = ((MonoObject *) ao)->vtable->klass;

	guint32 pos = 0;
	if (ao->bounds) {
		for (gint32 i = 0; i < m_class_get_rank (ac); i++) {
			gint32 idx = sp[i].data.i;
			gint32 lower = ao->bounds[i].lower_bound;
			guint32 len = ao->bounds[i].length;
			if (safe && (idx < lower || (guint32) (idx - lower) >= len))
				return -1;
			pos = (pos * len) + (guint32) (idx - lower);
		}
	} else {
		pos = sp[0].data.i;
		if (safe && pos >= ao->max_length)
			return -1;
	}
	return pos;
}

} // namespace mono::interp

#endif
