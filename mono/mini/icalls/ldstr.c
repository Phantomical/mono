/**
 * \file
 * ldstr: the interned string a user-string token stands for.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

MonoString*
ves_icall_mono_ldstr (MonoDomain *domain, MonoImage *image, guint32 idx)
{
	ERROR_DECL (error);
	MonoString *result = mono_ldstr_checked (domain, image, idx, error);
	mono_error_set_pending_exception (error);
	return result;
}

MonoString*
mono_helper_ldstr (MonoImage *image, guint32 idx)
{
	ERROR_DECL (error);
	MonoString *result = mono_ldstr_checked (mono_domain_get (), image, idx, error);
	mono_error_set_pending_exception (error);
	return result;
}

MonoString*
mono_helper_ldstr_mscorlib (guint32 idx)
{
	ERROR_DECL (error);
	MonoString *result = mono_ldstr_checked (mono_domain_get (), mono_defaults.corlib, idx, error);
	mono_error_set_pending_exception (error);
	return result;
}
