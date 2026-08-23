/**
 * \file
 * 64-bit multiply, for a target whose codegen does not emit one.
 *
 * Licensed under the MIT license. See LICENSE file in the project root for full license information.
 */
#include "icalls/icalls.h"

#if !defined(MONO_ARCH_NO_EMULATE_LONG_MUL_OPTS) || defined(MONO_ARCH_EMULATE_LONG_MUL_OVF_OPTS)

gint64
mono_llmult (gint64 a, gint64 b)
{
	return a * b;
}

guint64
mono_llmult_ovf_un (guint64 a, guint64 b)
{
	guint32 al = a;
	guint32 ah = a >> 32;
	guint32 bl = b;
	guint32 bh = b >> 32;
	guint64 res, t1;

	// fixme: this is incredible slow

	if (ah && bh)
		goto raise_exception;

	res = (guint64)al * (guint64)bl;

	t1 = (guint64)ah * (guint64)bl + (guint64)al * (guint64)bh;

	if (t1 > 0xffffffff)
		goto raise_exception;

	res += ((guint64)t1) << 32;

	return res;

 raise_exception:
	{
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
	}
	return 0;
}

guint64
mono_llmult_ovf (gint64 a, gint64 b)
{
	guint32 al = a;
	gint32 ah = a >> 32;
	guint32 bl = b;
	gint32 bh = b >> 32;
	/*
	Use Karatsuba algorithm where:
		a*b is: AhBh(R^2+R)+(Ah-Al)(Bl-Bh)R+AlBl(R+1)
		where Ah is the "high half" (most significant 32 bits) of a and
		where Al is the "low half" (least significant 32 bits) of a and
		where  Bh is the "high half" of b and Bl is the "low half" and
		where R is the Radix or "size of the half" (in our case 32 bits)

	Note, for the product of two 64 bit numbers to fit into a 64
	result, ah and/or bh must be 0.  This will save us from doing
	the AhBh term at all.

	Also note that we refactor so that we don't overflow 64 bits with
	intermediate results. So we use [(Ah-Al)(Bl-Bh)+AlBl]R+AlBl
	*/

	gint64 res, t1;
	gint32 sign;

	/* need to work with absoulte values, so find out what the
	   resulting sign will be and convert any negative numbers
	   from two's complement
	*/
	sign = ah ^ bh;
	if (ah < 0) {
		if (((guint32)ah == 0x80000000) && (al == 0)) {
			/* This has no two's complement */
			if (b == 0)
				return 0;
			else if (b == 1)
				return a;
			else
				goto raise_exception;
		}

		/* flip the bits and add 1 */
		ah ^= ~0;
		if (al ==  0)
			ah += 1;
		else {
			al ^= ~0;
			al +=1;
		}
	}

	if (bh < 0) {
		if (((guint32)bh == 0x80000000) && (bl == 0)) {
			/* This has no two's complement */
			if (a == 0)
				return 0;
			else if (a == 1)
				return b;
			else
				goto raise_exception;
		}

		/* flip the bits and add 1 */
		bh ^= ~0;
		if (bl ==  0)
			bh += 1;
		else {
			bl ^= ~0;
			bl +=1;
		}
	}

	/* we overflow for sure if both upper halves are greater
	   than zero because we would need to shift their
	   product 64 bits to the left and that will not fit
	   in a 64 bit result */
	if (ah && bh)
		goto raise_exception;
	if ((gint64)((gint64)ah * (gint64)bl) > (gint64)0x80000000 || (gint64)((gint64)al * (gint64)bh) > (gint64)0x80000000)
		goto raise_exception;

	/* do the AlBl term first */
	t1 = (gint64)al * (gint64)bl;

	res = t1;

	/* now do the [(Ah-Al)(Bl-Bh)+AlBl]R term */
	t1 += (gint64)(ah - al) * (gint64)(bl - bh);
	/* check for overflow */
	t1 <<= 32;
	if (t1 > (0x7FFFFFFFFFFFFFFFLL - res))
		goto raise_exception;

	res += t1;

	if (res < 0)
		goto raise_exception;

	if (sign < 0)
		return -res;
	else
		return res;

 raise_exception:
	{
		ERROR_DECL (error);
		mono_error_set_overflow (error);
		mono_error_set_pending_exception (error);
	}
	return 0;
}

#endif
