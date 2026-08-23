/**
 * \file
 * Float to integer, without an overflow check.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

guint64
mono_fconv_u8 (double v)
{
#if defined(TARGET_X86) || defined(TARGET_AMD64)
	const double two63 = 2147483648.0 * 4294967296.0;
	if (v < two63) {
		return (gint64)v;
	} else {
		return (gint64)(v - two63) + ((guint64)1 << 63);
	}
#else
	if (mono_isinf (v) || mono_isnan (v))
		return 0;
	return (guint64)v;
#endif
}

#ifdef MONO_ARCH_EMULATE_FCONV_TO_U8
guint64
mono_fconv_u8_2 (double v)
{
	// Separate from mono_fconv_u8 to avoid duplicate JIT icall.
	//
	// When there are duplicates, there is single instancing
	// against function address that breaks stuff. For example,
	// wrappers are only produced for one of them, breaking FullAOT.
	return mono_fconv_u8 (v);
}

guint64
mono_rconv_u8 (float v)
{
#if defined(TARGET_X86) || defined(TARGET_AMD64)
	const float two63 = 2147483648.0 * 4294967296.0;
	if (v < two63) {
		return (gint64)v;
	} else {
		return (gint64)(v - two63) + ((guint64)1 << 63);
	}
#else
	if (mono_isinf (v) || mono_isnan (v))
		return 0;
	return (guint64)v;
#endif
}
#endif

#ifdef MONO_ARCH_EMULATE_FCONV_TO_I8
gint64
mono_fconv_i8 (double v)
{
	return (gint64)v;
}
#endif

guint32
mono_fconv_u4 (double v)
{
	/* MS.NET behaves like this for some reason */
	if (mono_isinf (v) || mono_isnan (v))
		return 0;
	return (guint32)v;
}

#ifdef MONO_ARCH_EMULATE_FCONV_TO_U4
guint32
mono_fconv_u4_2 (double v)
{
	// Separate from mono_fconv_u4 to avoid duplicate JIT icall.
	//
	// When there are duplicates, there is single instancing
	// against function address that breaks stuff. For example,
	// wrappers are only produced for one of them, breaking FullAOT.
	return mono_fconv_u4 (v);
}

guint32
mono_rconv_u4 (float v)
{
	if (mono_isinf (v) || mono_isnan (v))
		return 0;
	return (guint32) v;
}
#endif

#ifdef MONO_ARCH_EMULATE_FCONV_TO_I8
gint64
mono_rconv_i8 (float v)
{
	return (gint64)v;
}
#endif
