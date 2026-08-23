/**
 * \file
 * Double arithmetic, for a target with no hardware floating point.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

#if defined(MONO_ARCH_EMULATE_MUL_DIV) || defined(MONO_ARCH_SOFT_FLOAT_FALLBACK)
double
mono_fdiv (double a, double b)
{
	return a / b;
}
#endif

#ifdef MONO_ARCH_SOFT_FLOAT_FALLBACK

double
mono_fsub (double a, double b)
{
	return a - b;
}

double
mono_fadd (double a, double b)
{
	return a + b;
}

double
mono_fmul (double a, double b)
{
	return a * b;
}

double
mono_fneg (double a)
{
	return -a;
}

#endif
