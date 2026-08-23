/**
 * \file
 * Integer to float, for a target whose codegen does not emit it.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

#ifdef MONO_ARCH_EMULATE_LCONV_TO_R8
double
mono_lconv_to_r8 (gint64 a)
{
	return (double)a;
}
#endif

#ifdef MONO_ARCH_EMULATE_LCONV_TO_R4
float
mono_lconv_to_r4 (gint64 a)
{
	return (float)a;
}
#endif

#ifdef MONO_ARCH_EMULATE_CONV_R8_UN
double
mono_conv_to_r8_un (guint32 a)
{
	return (double)a;
}
#endif

#ifdef MONO_ARCH_EMULATE_LCONV_TO_R8_UN
double
mono_lconv_to_r8_un (guint64 a)
{
	return (double)a;
}
#endif
