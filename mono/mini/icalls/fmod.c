/**
 * \file
 * rem on doubles, for a target whose codegen does not emit one.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

#ifdef MONO_ARCH_EMULATE_FREM
// Wrapper to avoid taking address of overloaded function.
double
mono_fmod (double a, double b)
{
	return fmod (a, b);
}
#endif
