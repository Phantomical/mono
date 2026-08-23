/**
 * \file
 * 64-bit divide and remainder, for a target whose codegen does not emit them.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

/* The guard is the long-multiply one, so a target gets the multiply and these
 * together or gets neither. */
#if !defined(MONO_ARCH_NO_EMULATE_LONG_MUL_OPTS) || defined(MONO_ARCH_EMULATE_LONG_MUL_OVF_OPTS)

gint64
mono_lldiv (gint64 a, gint64 b)
{
#ifdef MONO_ARCH_NEED_DIV_CHECK
	if (!b) {
		ERROR_DECL (error);
		mono_error_set_divide_by_zero (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
	else if (b == -1 && a == (-9223372036854775807LL - 1LL)) {
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
#endif
	return a / b;
}

gint64
mono_llrem (gint64 a, gint64 b)
{
#ifdef MONO_ARCH_NEED_DIV_CHECK
	if (!b) {
		ERROR_DECL (error);
		mono_error_set_divide_by_zero (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
	else if (b == -1 && a == (-9223372036854775807LL - 1LL)) {
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
#endif
	return a % b;
}

guint64
mono_lldiv_un (guint64 a, guint64 b)
{
#ifdef MONO_ARCH_NEED_DIV_CHECK
	if (!b) {
		ERROR_DECL (error);
		mono_error_set_divide_by_zero (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
#endif
	return a / b;
}

guint64
mono_llrem_un (guint64 a, guint64 b)
{
#ifdef MONO_ARCH_NEED_DIV_CHECK
	if (!b) {
		ERROR_DECL (error);
		mono_error_set_divide_by_zero (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
#endif
	return a % b;
}

#endif
