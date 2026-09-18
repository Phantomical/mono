// The conv.* family: every source and destination width pair, the unsigned
// forms, the overflow-checking forms, and conv.r.un.
//
// Every operand comes through a NoInlining helper, so the transform cannot fold
// the conversion away and leave the opcode untested.

using System;
using System.Runtime.CompilerServices;

[NoOpt]
public class Conversions {

	[MethodImpl (MethodImplOptions.NoInlining)] static int IdI4 (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static uint IdU4 (uint x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long IdI8 (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static ulong IdU8 (ulong x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static float IdR4 (float x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double IdR8 (double x) { return x; }

	// A store keeps a conversion alive when nothing else reads the result.
	static long sink;
	static ulong usink;

	// Integer to integer.

	public static int test_1_i1_from_i4_and_i8 ()
	{
		return (sbyte) IdI4 (0x1234) == 0x34 &&
		       (sbyte) IdI8 (0x1122334455667788L) == -120 ? 1 : 0;
	}

	public static int test_1_u1_from_i4_and_i8 ()
	{
		return (byte) IdI4 (-1) == 255 && (byte) IdI8 (0x1AAL) == 170 ? 1 : 0;
	}

	public static int test_1_i2_and_u2_from_i4 ()
	{
		return (short) IdI4 (0x1FFFF) == -1 && (ushort) IdI4 (-1) == 65535 ? 1 : 0;
	}

	public static int test_1_i2_and_u2_from_i8 ()
	{
		return (short) IdI8 (0x123456789ABCL) == -25924 &&
		       (ushort) IdI8 (-1L) == 65535 ? 1 : 0;
	}

	public static int test_7_i4_from_i8 ()
	{
		return (int) IdI8 (0x100000007L);
	}

	public static int test_1_u4_from_i8 ()
	{
		return (uint) IdI8 (-1L) == 4294967295u ? 1 : 0;
	}

	// conv.i8 copies the sign bit into the high word and conv.u8 clears it. A
	// long literal on the other side of a comparison compiles to the same
	// widening, so these read the sign and the low word instead.

	public static int test_1_i8_from_i4_sign_extends ()
	{
		long v = IdI4 (-1);

		return v < 0 && (int) v == -1 ? 1 : 0;
	}

	public static int test_1_i8_from_u4_zero_extends ()
	{
		long v = IdU4 (0xFFFFFFFFu);

		return v > 0 && (int) v == -1 ? 1 : 0;
	}

	// Floating point to integer. The conversion truncates, so it goes toward
	// zero and not toward the nearest integer.

	public static int test_1_i1_from_r8_and_r4_truncate_toward_zero ()
	{
		return (sbyte) IdR8 (-3.9) == -3 && (sbyte) IdR4 (3.9f) == 3 ? 1 : 0;
	}

	public static int test_1_u1_i2_u2_from_r8 ()
	{
		return (byte) IdR8 (255.9) == 255 &&
		       (short) IdR8 (-32768.9) == -32768 &&
		       (ushort) IdR8 (65535.9) == 65535 ? 1 : 0;
	}

	public static int test_1_u1_i2_u2_from_r4 ()
	{
		return (byte) IdR4 (200.7f) == 200 &&
		       (short) IdR4 (-1000.9f) == -1000 &&
		       (ushort) IdR4 (65535.5f) == 65535 ? 1 : 0;
	}

	public static int test_1_i4_from_r8_and_r4 ()
	{
		return (int) IdR8 (-2.7) == -2 && (int) IdR4 (-2.7f) == -2 ? 1 : 0;
	}

	public static int test_1_u4_from_r8_and_r4 ()
	{
		return (uint) IdR8 (4000000000.0) == 4000000000u &&
		       (uint) IdR4 (3000000000f) == 3000000000u ? 1 : 0;
	}

	public static int test_1_i8_from_r8_and_r4 ()
	{
		return (long) IdR8 (-1.9) == -1L &&
		       (long) IdR4 (1e10f) == 10000000000L ? 1 : 0;
	}

	// Above 2^63 the hardware instruction gives a signed result. conv.u8 from
	// a float must correct it.
	public static int test_1_u8_from_r8_and_r4_above_two63 ()
	{
		return (ulong) IdR8 (1e19) == 10000000000000000000UL &&
		       (ulong) IdR4 (1e19f) == 9999999980506447872UL ? 1 : 0;
	}

	// Integer to floating point, and one float width to the other.

	public static int test_1_r4_from_i4_and_i8_round ()
	{
		return (float) IdI4 (16777217) == 16777216.0f &&
		       (float) IdI8 (1099511627777L) == 1099511627776.0f ? 1 : 0;
	}

	public static int test_1_r8_from_i4_and_i8 ()
	{
		return (double) IdI4 (-123456789) == -123456789.0 &&
		       (double) IdI8 (9007199254740993L) == 9007199254740992.0 ? 1 : 0;
	}

	public static int test_1_r4_from_r8_narrows ()
	{
		return (double) (float) IdR8 (0.1) == 0.1 ? 0 : 1;
	}

	public static int test_1_r8_from_r4_widens ()
	{
		double d = IdR4 (0.1f);

		return d > 0.1 && d < 0.100001 ? 1 : 0;
	}

	// conv.r.un reads the source as unsigned. A source with its top bit set
	// becomes a large positive number and not a negative one.

	public static int test_1_r_un_from_i4 ()
	{
		return (double) IdU4 (4294967295u) == 4294967295.0 ? 1 : 0;
	}

	public static int test_1_r_un_from_i8 ()
	{
		return (double) IdU8 (18446744073709551615UL) == 18446744073709551616.0 ? 1 : 0;
	}

	// conv.i and conv.u, which are 64-bit on this target. The sign separates
	// them, for the same reason as the two tests above.
	public unsafe static int test_1_conv_i_sign_extends_conv_u_zero_extends ()
	{
		byte *from_signed = (byte *) IdI4 (-1);
		byte *from_unsigned = (byte *) IdU4 (0xFFFFFFFFu);

		return (long) from_signed < 0 && (long) from_unsigned > 0 &&
		       (int) from_unsigned == -1 ? 1 : 0;
	}

	// The checked conversions over integer sources.

	public static int test_2_ovf_i1_traps_from_i4_and_i8 ()
	{
		int traps = 0;

		try { checked { sink = (sbyte) IdI4 (128); } } catch (OverflowException) { traps++; }
		try { checked { sink = (sbyte) IdI8 (-129L); } } catch (OverflowException) { traps++; }
		return traps;
	}

	public static int test_2_ovf_i1_un_traps_from_u4_and_u8 ()
	{
		int traps = 0;

		try { checked { sink = (sbyte) IdU4 (200u); } } catch (OverflowException) { traps++; }
		try { checked { sink = (sbyte) IdU8 (128UL); } } catch (OverflowException) { traps++; }
		return traps;
	}

	public static int test_2_ovf_u1_traps_from_i4_and_i8 ()
	{
		int traps = 0;

		try { checked { sink = (byte) IdI4 (-1); } } catch (OverflowException) { traps++; }
		try { checked { sink = (byte) IdI8 (256L); } } catch (OverflowException) { traps++; }
		return traps;
	}

	public static int test_2_ovf_i2_traps_from_i4_and_i8 ()
	{
		int traps = 0;

		try { checked { sink = (short) IdI4 (32768); } } catch (OverflowException) { traps++; }
		try { checked { sink = (short) IdI8 (-32769L); } } catch (OverflowException) { traps++; }
		return traps;
	}

	public static int test_2_ovf_u2_traps_from_i4_and_i8 ()
	{
		int traps = 0;

		try { checked { sink = (ushort) IdI4 (-1); } } catch (OverflowException) { traps++; }
		try { checked { sink = (ushort) IdI8 (65536L); } } catch (OverflowException) { traps++; }
		return traps;
	}

	public static int test_2_ovf_i4_traps_from_i8_and_u8 ()
	{
		int traps = 0;

		try { checked { sink = (int) IdI8 (2147483648L); } } catch (OverflowException) { traps++; }
		try { checked { sink = (int) IdU8 (2147483648UL); } } catch (OverflowException) { traps++; }
		return traps;
	}

	public static int test_1_ovf_i4_un_traps_from_u4 ()
	{
		try {
			checked { sink = (int) IdU4 (0x80000000u); }
		} catch (OverflowException) {
			return 1;
		}
		return 0;
	}

	public static int test_2_ovf_u4_traps_from_i4_and_i8 ()
	{
		int traps = 0;

		try { checked { sink = (uint) IdI4 (-1); } } catch (OverflowException) { traps++; }
		try { checked { sink = (uint) IdI8 (4294967296L); } } catch (OverflowException) { traps++; }
		return traps;
	}

	public static int test_1_ovf_i8_un_traps_from_u8 ()
	{
		try {
			checked { sink = (long) IdU8 (9223372036854775808UL); }
		} catch (OverflowException) {
			return 1;
		}
		return 0;
	}

	public static int test_2_ovf_u8_traps_from_i4_and_i8 ()
	{
		int traps = 0;

		try { checked { usink = (ulong) IdI4 (-1); } } catch (OverflowException) { traps++; }
		try { checked { usink = (ulong) IdI8 (-1L); } } catch (OverflowException) { traps++; }
		return traps;
	}

	// conv.ovf.u4 from int64 accepts the full uint32 range, not only the part
	// an int32 holds.
	public static int test_2_ovf_in_range_i1_and_u4 ()
	{
		int ok = 0;

		checked {
			if ((sbyte) IdI4 (127) == 127)
				ok++;
			if ((uint) IdI8 (3000000000L) == 3000000000u)
				ok++;
		}
		return ok;
	}

	// The checked conversions over floating point sources.

	// The range check applies to the truncated value, so a source that is more
	// than the limit by a fraction still fits.
	public static int test_127_ovf_i1_from_r8_takes_a_fraction_over_the_limit ()
	{
		checked { return (sbyte) IdR8 (127.9); }
	}

	public static int test_3_ovf_traps_from_r8 ()
	{
		int traps = 0;

		try { checked { sink = (sbyte) IdR8 (128.0); } } catch (OverflowException) { traps++; }
		try { checked { sink = (ushort) IdR8 (-1.0); } } catch (OverflowException) { traps++; }
		try { checked { sink = (long) IdR8 (9.3e18); } } catch (OverflowException) { traps++; }
		return traps;
	}

	public static int test_2_ovf_traps_from_r4 ()
	{
		int traps = 0;

		try { checked { sink = (int) IdR4 (2147483648.0f); } } catch (OverflowException) { traps++; }
		try { checked { usink = (ulong) IdR4 (-1.5f); } } catch (OverflowException) { traps++; }
		return traps;
	}

	public static int test_3_ovf_i1_u2_u4_trap_from_r4 ()
	{
		int traps = 0;

		try { checked { sink = (sbyte) IdR4 (128.0f); } } catch (OverflowException) { traps++; }
		try { checked { sink = (ushort) IdR4 (65536.0f); } } catch (OverflowException) { traps++; }
		try { checked { sink = (uint) IdR4 (-1.5f); } } catch (OverflowException) { traps++; }
		return traps;
	}

	public static int test_1_ovf_u4_from_r8_boundary ()
	{
		checked { return (uint) IdR8 (4294967295.5) == 4294967295u ? 1 : 0; }
	}

	// A negative fraction truncates to zero, which is in range.
	public static int test_1_ovf_u8_from_r8_negative_fraction ()
	{
		checked { return (ulong) IdR8 (-0.5) == 0UL ? 1 : 0; }
	}

	public static int test_2_ovf_nan_and_infinity_trap ()
	{
		int traps = 0;

		try { checked { sink = (int) IdR8 (double.NaN); } } catch (OverflowException) { traps++; }
		try { checked { usink = (ulong) IdR8 (double.PositiveInfinity); } } catch (OverflowException) { traps++; }
		return traps;
	}

	/*
	 * ECMA-335 gives no result for an unchecked conversion of a floating point
	 * value that the destination cannot hold. The interpreter keeps what the
	 * x86 truncate instruction gives for the signed widths, and what
	 * mono_fconv_u4_2 () and mono_fconv_u8_2 () give for the unsigned ones. The
	 * tests below record that answer. The compiled engine gives the same one,
	 * through constrained_float_to_int () (mono/llvm/method-to-llvm/convert.cpp).
	 */

	public static int test_1_i4_from_nan_r8_is_int_min ()
	{
		return (int) IdR8 (double.NaN) == int.MinValue ? 1 : 0;
	}

	public static int test_1_i4_from_positive_infinity_is_int_min ()
	{
		return (int) IdR8 (double.PositiveInfinity) == int.MinValue ? 1 : 0;
	}

	public static int test_1_u4_from_out_of_range_r8_keeps_the_low_word ()
	{
		return (uint) IdR8 (5e9) == 705032704u ? 1 : 0;
	}

	public static int test_1_u4_from_negative_r8_wraps ()
	{
		return (uint) IdR8 (-1.5) == 4294967295u ? 1 : 0;
	}

	public static int test_1_i8_from_out_of_range_r8_is_long_min ()
	{
		return (long) IdR8 (1e30) == long.MinValue ? 1 : 0;
	}
}
