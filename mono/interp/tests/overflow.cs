// The checked opcodes: add.ovf, sub.ovf and mul.ovf with their .un forms, and
// the conv.ovf.* family from every source width.
//
// A test usually gives the widest value that stays in range, and the nearest
// value that does not, on both signs where the type has two. Each case sets one
// bit of the returned number, so a failure names the case that broke.

using System;
using System.Runtime.CompilerServices;

public class Overflow {

	// The transform folds constants and inlines a short callee, so an operand
	// that comes straight from a literal tests nothing.
	[MethodImpl (MethodImplOptions.NoInlining)] static int IdI4 (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static uint IdU4 (uint x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long IdI8 (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static ulong IdU8 (ulong x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static float IdR4 (float x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double IdR8 (double x) { return x; }

	static int SinkI4;
	static uint SinkU4;
	static long SinkI8;
	static ulong SinkU8;

	public static int test_7_add_ovf_i4 ()
	{
		int r = 0;

		if (checked (IdI4 (int.MaxValue - 1) + IdI4 (1)) == int.MaxValue)
			r |= 1;
		try { SinkI4 = checked (IdI4 (int.MaxValue) + IdI4 (1)); } catch (OverflowException) { r |= 2; }
		try { SinkI4 = checked (IdI4 (int.MinValue) + IdI4 (-1)); } catch (OverflowException) { r |= 4; }
		return r;
	}

	public static int test_7_add_ovf_i8 ()
	{
		int r = 0;

		if (checked (IdI8 (long.MaxValue - 1) + IdI8 (1)) == long.MaxValue)
			r |= 1;
		try { SinkI8 = checked (IdI8 (long.MaxValue) + IdI8 (1)); } catch (OverflowException) { r |= 2; }
		try { SinkI8 = checked (IdI8 (long.MinValue) + IdI8 (-1)); } catch (OverflowException) { r |= 4; }
		return r;
	}

	public static int test_3_add_ovf_un_i4 ()
	{
		int r = 0;

		if (checked (IdU4 (uint.MaxValue - 1) + IdU4 (1)) == uint.MaxValue)
			r |= 1;
		try { SinkU4 = checked (IdU4 (uint.MaxValue) + IdU4 (1)); } catch (OverflowException) { r |= 2; }
		return r;
	}

	public static int test_3_add_ovf_un_i8 ()
	{
		int r = 0;

		if (checked (IdU8 (ulong.MaxValue - 1) + IdU8 (1)) == ulong.MaxValue)
			r |= 1;
		try { SinkU8 = checked (IdU8 (ulong.MaxValue) + IdU8 (1)); } catch (OverflowException) { r |= 2; }
		return r;
	}

	public static int test_7_sub_ovf_i4 ()
	{
		int r = 0;

		if (checked (IdI4 (int.MinValue + 1) - IdI4 (1)) == int.MinValue)
			r |= 1;
		try { SinkI4 = checked (IdI4 (int.MinValue) - IdI4 (1)); } catch (OverflowException) { r |= 2; }
		try { SinkI4 = checked (IdI4 (int.MaxValue) - IdI4 (-1)); } catch (OverflowException) { r |= 4; }
		return r;
	}

	public static int test_7_sub_ovf_i8 ()
	{
		int r = 0;

		if (checked (IdI8 (long.MinValue + 1) - IdI8 (1)) == long.MinValue)
			r |= 1;
		try { SinkI8 = checked (IdI8 (long.MinValue) - IdI8 (1)); } catch (OverflowException) { r |= 2; }
		try { SinkI8 = checked (IdI8 (long.MaxValue) - IdI8 (-1)); } catch (OverflowException) { r |= 4; }
		return r;
	}

	public static int test_3_sub_ovf_un_i4 ()
	{
		int r = 0;

		if (checked (IdU4 (uint.MaxValue) - IdU4 (uint.MaxValue)) == 0)
			r |= 1;
		try { SinkU4 = checked (IdU4 (0) - IdU4 (1)); } catch (OverflowException) { r |= 2; }
		return r;
	}

	public static int test_3_sub_ovf_un_i8 ()
	{
		int r = 0;

		if (checked (IdU8 (ulong.MaxValue) - IdU8 (ulong.MaxValue)) == 0)
			r |= 1;
		try { SinkU8 = checked (IdU8 (0) - IdU8 (1)); } catch (OverflowException) { r |= 2; }
		return r;
	}

	public static int test_63_mul_ovf_i4 ()
	{
		int r = 0;

		if (checked (IdI4 (1073741823) * IdI4 (2)) == 2147483646)
			r |= 1;
		if (checked (IdI4 (-1073741824) * IdI4 (2)) == int.MinValue)
			r |= 2;
		try { SinkI4 = checked (IdI4 (1073741824) * IdI4 (2)); } catch (OverflowException) { r |= 4; }
		try { SinkI4 = checked (IdI4 (-1073741824) * IdI4 (3)); } catch (OverflowException) { r |= 8; }
		// Both operands fit, and the product is one more than int.MaxValue.
		try { SinkI4 = checked (IdI4 (int.MinValue) * IdI4 (-1)); } catch (OverflowException) { r |= 16; }
		if (checked (IdI4 (0) * IdI4 (int.MinValue)) == 0)
			r |= 32;
		return r;
	}

	public static int test_63_mul_ovf_i8 ()
	{
		int r = 0;

		if (checked (IdI8 (4611686018427387903L) * IdI8 (2)) == 9223372036854775806L)
			r |= 1;
		if (checked (IdI8 (-4611686018427387904L) * IdI8 (2)) == long.MinValue)
			r |= 2;
		try { SinkI8 = checked (IdI8 (4611686018427387904L) * IdI8 (2)); } catch (OverflowException) { r |= 4; }
		try { SinkI8 = checked (IdI8 (-4611686018427387904L) * IdI8 (3)); } catch (OverflowException) { r |= 8; }
		try { SinkI8 = checked (IdI8 (long.MinValue) * IdI8 (-1)); } catch (OverflowException) { r |= 16; }
		if (checked (IdI8 (0) * IdI8 (long.MinValue)) == 0)
			r |= 32;
		return r;
	}

	public static int test_7_mul_ovf_un_i4 ()
	{
		int r = 0;

		if (checked (IdU4 (2147483647u) * IdU4 (2)) == 4294967294u)
			r |= 1;
		try { SinkU4 = checked (IdU4 (2147483648u) * IdU4 (2)); } catch (OverflowException) { r |= 2; }
		if (checked (IdU4 (0) * IdU4 (uint.MaxValue)) == 0)
			r |= 4;
		return r;
	}

	public static int test_7_mul_ovf_un_i8 ()
	{
		int r = 0;

		if (checked (IdU8 (9223372036854775807UL) * IdU8 (2)) == 18446744073709551614UL)
			r |= 1;
		try { SinkU8 = checked (IdU8 (9223372036854775808UL) * IdU8 (2)); } catch (OverflowException) { r |= 2; }
		if (checked (IdU8 (0) * IdU8 (ulong.MaxValue)) == 0)
			r |= 4;
		return r;
	}

	// A checked negation is a subtraction from zero, so it uses sub.ovf.
	public static int test_15_negate_checked ()
	{
		int r = 0;

		if (checked (-IdI4 (int.MinValue + 1)) == int.MaxValue)
			r |= 1;
		try { SinkI4 = checked (-IdI4 (int.MinValue)); } catch (OverflowException) { r |= 2; }
		if (checked (-IdI8 (long.MinValue + 1)) == long.MaxValue)
			r |= 4;
		try { SinkI8 = checked (-IdI8 (long.MinValue)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_15_conv_ovf_i1_from_i4 ()
	{
		int r = 0;

		if (checked ((sbyte) IdI4 (127)) == 127) r |= 1;
		if (checked ((sbyte) IdI4 (-128)) == -128) r |= 2;
		try { SinkI4 = checked ((sbyte) IdI4 (128)); } catch (OverflowException) { r |= 4; }
		try { SinkI4 = checked ((sbyte) IdI4 (-129)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_31_conv_ovf_i1_from_i8 ()
	{
		int r = 0;

		if (checked ((sbyte) IdI8 (127)) == 127) r |= 1;
		if (checked ((sbyte) IdI8 (-128)) == -128) r |= 2;
		try { SinkI4 = checked ((sbyte) IdI8 (128)); } catch (OverflowException) { r |= 4; }
		try { SinkI4 = checked ((sbyte) IdI8 (-129)); } catch (OverflowException) { r |= 8; }
		// The range test reads all 64 bits, not the byte that would survive.
		try { SinkI4 = checked ((sbyte) IdI8 (0x100000001L)); } catch (OverflowException) { r |= 16; }
		return r;
	}

	public static int test_7_conv_ovf_i1_from_u4 ()
	{
		int r = 0;

		if (checked ((sbyte) IdU4 (127)) == 127) r |= 1;
		try { SinkI4 = checked ((sbyte) IdU4 (128)); } catch (OverflowException) { r |= 2; }
		try { SinkI4 = checked ((sbyte) IdU4 (2147483648u)); } catch (OverflowException) { r |= 4; }
		return r;
	}

	public static int test_7_conv_ovf_i1_from_u8 ()
	{
		int r = 0;

		if (checked ((sbyte) IdU8 (127)) == 127) r |= 1;
		try { SinkI4 = checked ((sbyte) IdU8 (128)); } catch (OverflowException) { r |= 2; }
		try { SinkI4 = checked ((sbyte) IdU8 (ulong.MaxValue)); } catch (OverflowException) { r |= 4; }
		return r;
	}

	public static int test_15_conv_ovf_u1_from_i4 ()
	{
		int r = 0;

		if (checked ((byte) IdI4 (255)) == 255) r |= 1;
		if (checked ((byte) IdI4 (0)) == 0) r |= 2;
		try { SinkI4 = checked ((byte) IdI4 (256)); } catch (OverflowException) { r |= 4; }
		try { SinkI4 = checked ((byte) IdI4 (-1)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_15_conv_ovf_u1_from_i8_and_u8 ()
	{
		int r = 0;

		if (checked ((byte) IdI8 (255)) == 255) r |= 1;
		try { SinkI4 = checked ((byte) IdI8 (256)); } catch (OverflowException) { r |= 2; }
		try { SinkI4 = checked ((byte) IdI8 (-1)); } catch (OverflowException) { r |= 4; }
		try { SinkI4 = checked ((byte) IdU8 (ulong.MaxValue)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_3_conv_ovf_u1_from_u4 ()
	{
		int r = 0;

		if (checked ((byte) IdU4 (255)) == 255) r |= 1;
		try { SinkI4 = checked ((byte) IdU4 (256)); } catch (OverflowException) { r |= 2; }
		return r;
	}

	public static int test_15_conv_ovf_i2_from_i4 ()
	{
		int r = 0;

		if (checked ((short) IdI4 (32767)) == 32767) r |= 1;
		if (checked ((short) IdI4 (-32768)) == -32768) r |= 2;
		try { SinkI4 = checked ((short) IdI4 (32768)); } catch (OverflowException) { r |= 4; }
		try { SinkI4 = checked ((short) IdI4 (-32769)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_15_conv_ovf_i2_from_i8 ()
	{
		int r = 0;

		if (checked ((short) IdI8 (32767)) == 32767) r |= 1;
		if (checked ((short) IdI8 (-32768)) == -32768) r |= 2;
		try { SinkI4 = checked ((short) IdI8 (32768)); } catch (OverflowException) { r |= 4; }
		try { SinkI4 = checked ((short) IdI8 (-32769)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_15_conv_ovf_i2_from_unsigned ()
	{
		int r = 0;

		if (checked ((short) IdU4 (32767)) == 32767) r |= 1;
		try { SinkI4 = checked ((short) IdU4 (32768)); } catch (OverflowException) { r |= 2; }
		if (checked ((short) IdU8 (32767)) == 32767) r |= 4;
		try { SinkI4 = checked ((short) IdU8 (ulong.MaxValue)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_63_conv_ovf_u2_from_integers ()
	{
		int r = 0;

		if (checked ((ushort) IdI4 (65535)) == 65535) r |= 1;
		try { SinkI4 = checked ((ushort) IdI4 (65536)); } catch (OverflowException) { r |= 2; }
		try { SinkI4 = checked ((ushort) IdI4 (-1)); } catch (OverflowException) { r |= 4; }
		if (checked ((ushort) IdI8 (65535)) == 65535) r |= 8;
		try { SinkI4 = checked ((ushort) IdI8 (-1)); } catch (OverflowException) { r |= 16; }
		try { SinkI4 = checked ((ushort) IdU8 (65536)); } catch (OverflowException) { r |= 32; }
		return r;
	}

	public static int test_15_conv_ovf_i4_from_i8 ()
	{
		int r = 0;

		if (checked ((int) IdI8 (2147483647L)) == int.MaxValue) r |= 1;
		if (checked ((int) IdI8 (-2147483648L)) == int.MinValue) r |= 2;
		try { SinkI4 = checked ((int) IdI8 (2147483648L)); } catch (OverflowException) { r |= 4; }
		try { SinkI4 = checked ((int) IdI8 (-2147483649L)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_15_conv_ovf_i4_from_unsigned ()
	{
		int r = 0;

		if (checked ((int) IdU4 (2147483647u)) == int.MaxValue) r |= 1;
		try { SinkI4 = checked ((int) IdU4 (2147483648u)); } catch (OverflowException) { r |= 2; }
		if (checked ((int) IdU8 (2147483647UL)) == int.MaxValue) r |= 4;
		try { SinkI4 = checked ((int) IdU8 (2147483648UL)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_31_conv_ovf_u4_from_signed ()
	{
		int r = 0;

		if (checked ((uint) IdI4 (int.MaxValue)) == 2147483647u) r |= 1;
		try { SinkU4 = checked ((uint) IdI4 (-1)); } catch (OverflowException) { r |= 2; }
		if (checked ((uint) IdI8 (4294967295L)) == uint.MaxValue) r |= 4;
		try { SinkU4 = checked ((uint) IdI8 (4294967296L)); } catch (OverflowException) { r |= 8; }
		try { SinkU4 = checked ((uint) IdI8 (-1L)); } catch (OverflowException) { r |= 16; }
		return r;
	}

	public static int test_15_conv_ovf_wide_from_u8 ()
	{
		int r = 0;

		if (checked ((uint) IdU8 (4294967295UL)) == uint.MaxValue) r |= 1;
		try { SinkU4 = checked ((uint) IdU8 (4294967296UL)); } catch (OverflowException) { r |= 2; }
		if (checked ((long) IdU8 (9223372036854775807UL)) == long.MaxValue) r |= 4;
		try { SinkI8 = checked ((long) IdU8 (9223372036854775808UL)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_15_conv_ovf_u8_from_signed ()
	{
		int r = 0;

		if (checked ((ulong) IdI4 (int.MaxValue)) == 2147483647UL) r |= 1;
		try { SinkU8 = checked ((ulong) IdI4 (-1)); } catch (OverflowException) { r |= 2; }
		if (checked ((ulong) IdI8 (long.MaxValue)) == 9223372036854775807UL) r |= 4;
		try { SinkU8 = checked ((ulong) IdI8 (-1L)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_15_conv_ovf_i1_from_r8 ()
	{
		int r = 0;

		if (checked ((sbyte) IdR8 (127.0)) == 127) r |= 1;
		if (checked ((sbyte) IdR8 (-128.0)) == -128) r |= 2;
		try { SinkI4 = checked ((sbyte) IdR8 (128.0)); } catch (OverflowException) { r |= 4; }
		try { SinkI4 = checked ((sbyte) IdR8 (-129.0)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_15_conv_ovf_i1_from_r4 ()
	{
		int r = 0;

		if (checked ((sbyte) IdR4 (127f)) == 127) r |= 1;
		if (checked ((sbyte) IdR4 (-128f)) == -128) r |= 2;
		try { SinkI4 = checked ((sbyte) IdR4 (128f)); } catch (OverflowException) { r |= 4; }
		try { SinkI4 = checked ((sbyte) IdR4 (-129f)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_15_conv_ovf_u1_from_float ()
	{
		int r = 0;

		if (checked ((byte) IdR8 (255.0)) == 255) r |= 1;
		try { SinkI4 = checked ((byte) IdR8 (256.0)); } catch (OverflowException) { r |= 2; }
		try { SinkI4 = checked ((byte) IdR8 (-1.0)); } catch (OverflowException) { r |= 4; }
		if (checked ((byte) IdR4 (255f)) == 255) r |= 8;
		return r;
	}

	public static int test_15_conv_ovf_i2_from_float ()
	{
		int r = 0;

		if (checked ((short) IdR8 (32767.0)) == 32767) r |= 1;
		try { SinkI4 = checked ((short) IdR8 (32768.0)); } catch (OverflowException) { r |= 2; }
		if (checked ((short) IdR4 (-32768f)) == -32768) r |= 4;
		try { SinkI4 = checked ((short) IdR4 (-32769f)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_15_conv_ovf_u2_from_float ()
	{
		int r = 0;

		if (checked ((ushort) IdR8 (65535.0)) == 65535) r |= 1;
		try { SinkI4 = checked ((ushort) IdR8 (65536.0)); } catch (OverflowException) { r |= 2; }
		try { SinkI4 = checked ((ushort) IdR4 (-1f)); } catch (OverflowException) { r |= 4; }
		if (checked ((ushort) IdR4 (65535f)) == 65535) r |= 8;
		return r;
	}

	public static int test_31_conv_ovf_i4_from_r8 ()
	{
		int r = 0;

		if (checked ((int) IdR8 (2147483647.0)) == int.MaxValue) r |= 1;
		if (checked ((int) IdR8 (-2147483648.0)) == int.MinValue) r |= 2;
		try { SinkI4 = checked ((int) IdR8 (2147483648.0)); } catch (OverflowException) { r |= 4; }
		try { SinkI4 = checked ((int) IdR8 (-2147483649.0)); } catch (OverflowException) { r |= 8; }
		try { SinkI4 = checked ((int) IdR8 (double.NaN)); } catch (OverflowException) { r |= 16; }
		return r;
	}

	public static int test_31_conv_ovf_i4_from_r4 ()
	{
		int r = 0;

		// 2147483520 is the largest float below 2^31, and -2147483904 is the
		// first float below -2^31.
		if (checked ((int) IdR4 (2147483520f)) == 2147483520) r |= 1;
		if (checked ((int) IdR4 (-2147483648f)) == int.MinValue) r |= 2;
		try { SinkI4 = checked ((int) IdR4 (2147483648f)); } catch (OverflowException) { r |= 4; }
		try { SinkI4 = checked ((int) IdR4 (-2147483904f)); } catch (OverflowException) { r |= 8; }
		try { SinkI4 = checked ((int) IdR4 (float.PositiveInfinity)); } catch (OverflowException) { r |= 16; }
		return r;
	}

	public static int test_15_conv_ovf_u4_from_float ()
	{
		int r = 0;

		if (checked ((uint) IdR8 (4294967295.0)) == uint.MaxValue) r |= 1;
		try { SinkU4 = checked ((uint) IdR8 (4294967296.0)); } catch (OverflowException) { r |= 2; }
		try { SinkU4 = checked ((uint) IdR8 (-1.0)); } catch (OverflowException) { r |= 4; }
		// 4294967040 is the largest float below 2^32.
		if (checked ((uint) IdR4 (4294967040f)) == 4294967040u) r |= 8;
		return r;
	}

	public static int test_31_conv_ovf_i8_from_r8 ()
	{
		int r = 0;

		// 9223372036854774784 is the largest double below 2^63, and
		// -9223372036854777856 is the first double below -2^63.
		if (checked ((long) IdR8 (9223372036854774784.0)) == 9223372036854774784L) r |= 1;
		if (checked ((long) IdR8 (-9223372036854775808.0)) == long.MinValue) r |= 2;
		try { SinkI8 = checked ((long) IdR8 (9223372036854775808.0)); } catch (OverflowException) { r |= 4; }
		try { SinkI8 = checked ((long) IdR8 (-9223372036854777856.0)); } catch (OverflowException) { r |= 8; }
		try { SinkI8 = checked ((long) IdR8 (double.NaN)); } catch (OverflowException) { r |= 16; }
		return r;
	}

	public static int test_7_conv_ovf_i8_from_r4 ()
	{
		int r = 0;

		if (checked ((long) IdR4 (-9223372036854775808f)) == long.MinValue) r |= 1;
		try { SinkI8 = checked ((long) IdR4 (9223372036854775808f)); } catch (OverflowException) { r |= 2; }
		try { SinkI8 = checked ((long) IdR4 (float.NegativeInfinity)); } catch (OverflowException) { r |= 4; }
		return r;
	}

	public static int test_15_conv_ovf_u8_from_r8 ()
	{
		int r = 0;

		// 18446744073709549568 is the largest double below 2^64.
		if (checked ((ulong) IdR8 (18446744073709549568.0)) == 18446744073709549568UL) r |= 1;
		if (checked ((ulong) IdR8 (0.0)) == 0) r |= 2;
		try { SinkU8 = checked ((ulong) IdR8 (18446744073709551616.0)); } catch (OverflowException) { r |= 4; }
		try { SinkU8 = checked ((ulong) IdR8 (-1.0)); } catch (OverflowException) { r |= 8; }
		return r;
	}

	public static int test_7_conv_ovf_u8_from_r4 ()
	{
		int r = 0;

		// 18446742974197923840 is the largest float below 2^64.
		if (checked ((ulong) IdR4 (18446742974197923840f)) == 18446742974197923840UL) r |= 1;
		try { SinkU8 = checked ((ulong) IdR4 (18446744073709551616f)); } catch (OverflowException) { r |= 2; }
		try { SinkU8 = checked ((ulong) IdR4 (-1f)); } catch (OverflowException) { r |= 4; }
		return r;
	}

	// A value outside the destination range by less than one still converts.
	// Only the integer part has to fit.
	public static int test_15_conv_ovf_only_the_integer_part_must_fit ()
	{
		int r = 0;

		if (checked ((sbyte) IdR8 (-128.5)) == -128) r |= 1;
		if (checked ((sbyte) IdR8 (127.9)) == 127) r |= 2;
		if (checked ((sbyte) IdR4 (-128.5f)) == -128) r |= 4;
		if (checked ((sbyte) IdR4 (127.9f)) == 127) r |= 8;
		return r;
	}

	public static int test_3_conv_ovf_unsigned_truncates_a_small_negative ()
	{
		int r = 0;

		if (checked ((byte) IdR8 (-0.5)) == 0) r |= 1;
		if (checked ((uint) IdR4 (-0.5f)) == 0) r |= 2;
		return r;
	}

	public static int test_3_conv_ovf_i4_fraction_at_the_boundary ()
	{
		int r = 0;

		if (checked ((int) IdR8 (2147483647.5)) == int.MaxValue) r |= 1;
		if (checked ((int) IdR8 (-2147483648.5)) == int.MinValue) r |= 2;
		return r;
	}

	public static int test_3_conv_ovf_u4_fraction_at_the_boundary ()
	{
		int r = 0;

		if (checked ((uint) IdR8 (4294967295.5)) == uint.MaxValue) r |= 1;
		if (checked ((uint) IdR8 (-0.75)) == 0) r |= 2;
		return r;
	}

	// NaN is neither more than the low bound nor less than the high one, so a
	// range test written as a pair of comparisons rejects it.
	public static int test_31_conv_ovf_nan_and_infinity ()
	{
		int r = 0;

		try { SinkI4 = checked ((byte) IdR8 (double.NaN)); } catch (OverflowException) { r |= 1; }
		try { SinkU4 = checked ((uint) IdR8 (double.NaN)); } catch (OverflowException) { r |= 2; }
		try { SinkU8 = checked ((ulong) IdR8 (double.NaN)); } catch (OverflowException) { r |= 4; }
		try { SinkU8 = checked ((ulong) IdR8 (double.PositiveInfinity)); } catch (OverflowException) { r |= 8; }
		try { SinkI4 = checked ((short) IdR4 (float.NaN)); } catch (OverflowException) { r |= 16; }
		return r;
	}

	public static int test_1_overflow_throws_an_overflow_exception ()
	{
		try {
			SinkI4 = checked (IdI4 (int.MaxValue) + IdI4 (1));
		} catch (Exception e) {
			return e is OverflowException ? 1 : 0;
		}
		return 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int AddChecked (int a, int b) { return checked (a + b); }

	public static int test_3_overflow_unwinds_out_of_a_callee ()
	{
		int r = 0;

		if (AddChecked (IdI4 (1), IdI4 (2)) == 3) r |= 1;
		try { SinkI4 = AddChecked (IdI4 (int.MaxValue), IdI4 (1)); } catch (OverflowException) { r |= 2; }
		return r;
	}

	public static int test_7_finally_runs_while_an_overflow_unwinds ()
	{
		int marks = 0;

		try {
			try {
				SinkI4 = checked (IdI4 (int.MaxValue) * IdI4 (2));
			} finally {
				marks |= 1;
			}
		} catch (OverflowException) {
			marks |= 2;
		}
		if (checked (IdI4 (2) * IdI4 (3)) == 6)
			marks |= 4;
		return marks;
	}
}
