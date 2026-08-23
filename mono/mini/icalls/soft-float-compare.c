/**
 * \file
 * Double comparison, for a target with no hardware floating point.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

#ifdef MONO_ARCH_SOFT_FLOAT_FALLBACK

gboolean
mono_fcmp_eq (double a, double b)
{
	return a == b;
}

gboolean
mono_fcmp_ge (double a, double b)
{
	return a >= b;
}

gboolean
mono_fcmp_gt (double a, double b)
{
	return a > b;
}

gboolean
mono_fcmp_le (double a, double b)
{
	return a <= b;
}

gboolean
mono_fcmp_lt (double a, double b)
{
	return a < b;
}

gboolean
mono_fcmp_ne_un (double a, double b)
{
	return isunordered (a, b) || a != b;
}

gboolean
mono_fcmp_ge_un (double a, double b)
{
	return isunordered (a, b) || a >= b;
}

gboolean
mono_fcmp_gt_un (double a, double b)
{
	return isunordered (a, b) || a > b;
}

gboolean
mono_fcmp_le_un (double a, double b)
{
	return isunordered (a, b) || a <= b;
}

gboolean
mono_fcmp_lt_un (double a, double b)
{
	return isunordered (a, b) || a < b;
}

gboolean
mono_fceq (double a, double b)
{
	return a == b;
}

gboolean
mono_fcgt (double a, double b)
{
	return a > b;
}

gboolean
mono_fcgt_un (double a, double b)
{
	return isunordered (a, b) || a > b;
}

gboolean
mono_fclt (double a, double b)
{
	return a < b;
}

gboolean
mono_fclt_un (double a, double b)
{
	return isunordered (a, b) || a < b;
}

#endif
