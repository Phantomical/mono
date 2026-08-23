/**
 * \file
 * A copy whose type only the caller's generic context names.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

void
mono_gsharedvt_value_copy (gpointer dest, gpointer src, MonoClass *klass)
{
	if (m_class_is_valuetype (klass))
		mono_value_copy_internal (dest, src, klass);
	else
        mono_gc_wbarrier_generic_store_internal (dest, *(MonoObject**)src);
}
