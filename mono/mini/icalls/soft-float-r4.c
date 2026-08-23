/**
 * \file
 * Single-precision loads and stores, for a target with no hardware floating point.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

#ifdef MONO_ARCH_SOFT_FLOAT_FALLBACK

double
mono_fload_r4 (float *ptr)
{
	return *ptr;
}

void
mono_fstore_r4 (double val, float *ptr)
{
	*ptr = (float)val;
}

/* returns the integer bitpattern that is passed in the regs or stack */
guint32
mono_fload_r4_arg (double val)
{
	float v = (float)val;
	return *(guint32*)&v;
}

#endif
