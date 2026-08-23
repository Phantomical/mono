/**
 * \file
 * 32-bit multiply, for a target whose codegen does not emit one.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

#if defined(MONO_ARCH_EMULATE_MUL_DIV) || defined(MONO_ARCH_EMULATE_MUL_OVF)

gint32
mono_imul (gint32 a, gint32 b)
{
	return a * b;
}

gint32
mono_imul_ovf (gint32 a, gint32 b)
{
	const gint64 res = (gint64)a * (gint64)b;

	if ((res > 0x7fffffffL) || (res < -2147483648LL)) {
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
		return 0;
	}

	return res;
}

gint32
mono_imul_ovf_un (guint32 a, guint32 b)
{
	const guint64 res = (guint64)a * (guint64)b;

	if (res >> 32) {
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
		return 0;
	}

	return res;
}

#endif
