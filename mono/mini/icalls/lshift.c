/**
 * \file
 * 64-bit shifts, for a target whose codegen does not emit them.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

#ifndef MONO_ARCH_NO_EMULATE_LONG_SHIFT_OPS

guint64
mono_lshl (guint64 a, gint32 shamt)
{
	const guint64 res = a << (shamt & 0x7f);

	/*printf ("TESTL %" PRId64 " << %d = %" PRId64 "\n", a, shamt, (guint64)res);*/

	return res;
}

guint64
mono_lshr_un (guint64 a, gint32 shamt)
{
	const guint64 res = a >> (shamt & 0x7f);

	/*printf ("TESTR %" PRId64 " >> %d = %" PRId64 "\n", a, shamt, (guint64)res);*/

	return res;
}

gint64
mono_lshr (gint64 a, gint32 shamt)
{
	const gint64 res = a >> (shamt & 0x7f);

	/*printf ("TESTR %" PRId64 " >> %d = %" PRId64 "\n", a, shamt, (guint64)res);*/

	return res;
}

#endif
