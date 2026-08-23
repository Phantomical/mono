/**
 * \file
 * ckfinite: throws on an infinity or a NaN, and returns the value otherwise.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

double
mono_ckfinite (double d)
{
	if (mono_isinf (d) || mono_isnan (d))
		mono_set_pending_exception (mono_get_exception_arithmetic ());
	return d;
}
