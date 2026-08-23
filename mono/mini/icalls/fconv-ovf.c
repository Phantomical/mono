/**
 * \file
 * Float to integer, which throws when the value does not fit.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

gint64
mono_fconv_ovf_i8 (double v)
{
	const gint64 res = (gint64)v;

	if (mono_isnan (v) || mono_trunc (v) != res) {
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
	return res;
}

guint64
mono_fconv_ovf_u8 (double v)
{
	guint64 res;

/*
 * The soft-float implementation of some ARM devices have a buggy guin64 to double
 * conversion that it looses precision even when the integer if fully representable
 * as a double.
 *
 * This was found with 4294967295ull, converting to double and back looses one bit of precision.
 *
 * To work around this issue we test for value boundaries instead.
 */
#if defined(__arm__) && defined(MONO_ARCH_SOFT_FLOAT_FALLBACK)
	if (mono_isnan (v) || !(v >= -0.5 && v <= ULLONG_MAX+0.5)) {
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
	res = (guint64)v;
#else
	res = (guint64)v;
	if (mono_isnan (v) || mono_trunc (v) != res) {
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
#endif
	return res;
}

gint64
mono_rconv_ovf_i8 (float v)
{
	const gint64 res = (gint64)v;

	if (mono_isnan (v) || mono_trunc (v) != res) {
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
	return res;
}

guint64
mono_rconv_ovf_u8 (float v)
{
	guint64 res;

	res = (guint64)v;
	if (mono_isnan (v) || mono_trunc (v) != res) {
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
		return 0;
	}
	return res;
}
