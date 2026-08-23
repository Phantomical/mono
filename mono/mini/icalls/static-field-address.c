/**
 * \file
 * Where a static field lives in a domain.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

gpointer
mono_class_static_field_address (MonoDomain *domain, MonoClassField *field)
{
	ERROR_DECL (error);
	MonoVTable *vtable;
	gpointer addr;

	//printf ("SFLDA0 %s.%s::%s %d\n", field->parent->name_space, field->parent->name, field->name, field->offset, field->parent->inited);

	mono_class_init_internal (field->parent);

	vtable = mono_class_vtable_checked (domain, field->parent, error);
	if (!is_ok (error)) {
		mono_error_set_pending_exception (error);
		return NULL;
	}
	if (!vtable->initialized) {
		if (!mono_runtime_class_init_full (vtable, error)) {
			mono_error_set_pending_exception (error);
			return NULL;
		}
	}

	//printf ("SFLDA1 %p\n", (char*)vtable->data + field->offset);

	if (field->offset == -1) {
		/* Special static */
		g_assert (domain->special_static_fields);
		mono_domain_lock (domain);
		addr = g_hash_table_lookup (domain->special_static_fields, field);
		mono_domain_unlock (domain);
		addr = mono_get_special_static_data (GPOINTER_TO_UINT (addr));
	} else {
		addr = (char*)mono_vtable_get_static_field_data (vtable) + field->offset;
	}
	return addr;
}
