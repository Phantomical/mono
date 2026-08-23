/**
 * \file
 * 32-bit divide and remainder, for a target whose codegen does not emit them.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

#if defined(MONO_ARCH_EMULATE_MUL_DIV) || defined(MONO_ARCH_EMULATE_DIV)

gint32
mono_idiv (gint32 a, gint32 b)
{
#ifdef MONO_ARCH_NEED_DIV_CHECK
	if (!b) {
		ERROR_DECL (error);
		mono_error_set_divide_by_zero (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
	else if (b == -1 && a == (0x80000000)) {
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
#endif
	return a / b;
}

guint32
mono_idiv_un (guint32 a, guint32 b)
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

gint32
mono_irem (gint32 a, gint32 b)
{
#ifdef MONO_ARCH_NEED_DIV_CHECK
	if (!b) {
		ERROR_DECL (error);
		mono_error_set_divide_by_zero (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
	else if (b == -1 && a == (0x80000000)) {
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
#endif
	return a % b;
}

guint32
mono_irem_un (guint32 a, guint32 b)
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
