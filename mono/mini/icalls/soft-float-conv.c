/**
 * \file
 * Conversions to and from double, for a target with no hardware floating point.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

#ifdef MONO_ARCH_SOFT_FLOAT_FALLBACK

double
mono_fconv_r4 (double a)
{
	return (float)a;
}

double
mono_conv_to_r8 (int a)
{
	return (double)a;
}

double
mono_conv_to_r4 (int a)
{
	return (double)(float)a;
}

gint8
mono_fconv_i1 (double a)
{
	return (gint8)a;
}

gint16
mono_fconv_i2 (double a)
{
	return (gint16)a;
}

gint32
mono_fconv_i4 (double a)
{
	return (gint32)a;
}

guint8
mono_fconv_u1 (double a)
{
	return (guint8)a;
}

guint16
mono_fconv_u2 (double a)
{
	return (guint16)a;
}

#endif
