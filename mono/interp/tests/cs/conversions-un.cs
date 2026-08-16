// The conv.* forms conversions.cs leaves out: a native int source or
// destination, an unsigned source at each destination width, and a source the
// transform already knows the value of.
//
// Most operands come through a NoInlining helper, so the transform cannot fold
// them. The tests at the end of the file want the opposite. They take the
// operand from a plain local, because they exercise the fold table.

using System;
using System.Runtime.CompilerServices;

[NoOpt]
public class ConversionsUn {

	[MethodImpl (MethodImplOptions.NoInlining)] static int IdI4 (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static uint IdU4 (uint x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long IdI8 (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static ulong IdU8 (ulong x) { return x; }

	// A store keeps a conversion alive when nothing else reads the result.
	static long sink;
	static ulong usink;
	unsafe static void *psink;

	// A cast to a pointer type is how C# reaches conv.u. The destination
	// is 64 bit here, so a 64 bit source keeps every bit and the sign with it.
	public unsafe static int test_1_conv_native_from_i8_and_u8 ()
	{
		void *from_signed = (void *) IdI8 (-1L);
		void *from_unsigned = (void *) IdU8 (0x123456789ABCUL);

		return (long) from_signed == -1L && (ulong) from_unsigned == 0x123456789ABCUL ? 1 : 0;
	}

	// A cast to a pointer is unsigned, so a checked one gives conv.ovf.u over a
	// signed source. Only a negative value is out of range for the destination.
	public unsafe static int test_4_ovf_native_from_i4_and_i8 ()
	{
		int ok = 0;

		checked { psink = (void *) IdI4 (5); }
		if ((ulong) psink == 5UL)
			ok++;
		try { checked { psink = (void *) IdI4 (-5); } } catch (OverflowException) { ok++; }

		checked { psink = (void *) IdI8 (0x7FFFFFFFFFFFFFFFL); }
		if ((ulong) psink == 0x7FFFFFFFFFFFFFFFUL)
			ok++;
		try { checked { psink = (void *) IdI8 (-1L); } } catch (OverflowException) { ok++; }
		return ok;
	}

	// conv.ovf.u.un over a uint64 source. The whole range fits, so the opcode
	// becomes nothing at all.
	public unsafe static int test_1_ovf_native_un_from_u8 ()
	{
		checked { psink = (void *) IdU8 (0xFFFFFFFFFFFFFFFFUL); }

		return (ulong) psink == 0xFFFFFFFFFFFFFFFFUL ? 1 : 0;
	}

	// conv.ovf.<width>.un at the top of each destination range, from a uint32
	// source and then from a uint64 one.

	public static int test_5_ovf_un_in_range_from_u4 ()
	{
		int ok = 0;

		checked {
			if ((sbyte) IdU4 (127u) == 127)
				ok++;
			if ((byte) IdU4 (255u) == 255)
				ok++;
			if ((short) IdU4 (32767u) == 32767)
				ok++;
			if ((ushort) IdU4 (65535u) == 65535)
				ok++;
			if ((int) IdU4 (2147483647u) == 2147483647)
				ok++;
		}
		return ok;
	}

	public static int test_7_ovf_un_in_range_from_u8 ()
	{
		int ok = 0;

		checked {
			if ((sbyte) IdU8 (127UL) == 127)
				ok++;
			if ((byte) IdU8 (255UL) == 255)
				ok++;
			if ((short) IdU8 (32767UL) == 32767)
				ok++;
			if ((ushort) IdU8 (65535UL) == 65535)
				ok++;
			if ((int) IdU8 (2147483647UL) == 2147483647)
				ok++;
			if ((uint) IdU8 (4294967295UL) == 4294967295u)
				ok++;
			if ((long) IdU8 (9223372036854775807UL) == 9223372036854775807L)
				ok++;
		}
		return ok;
	}

	// conv.ovf.u1.un, conv.ovf.i2.un, conv.ovf.u2.un and conv.ovf.u4.un, each
	// one past the top of its range.
	public static int test_4_ovf_un_traps ()
	{
		int traps = 0;

		try { checked { sink = (byte) IdU4 (256u); } } catch (OverflowException) { traps++; }
		try { checked { sink = (short) IdU4 (32768u); } } catch (OverflowException) { traps++; }
		try { checked { sink = (ushort) IdU8 (65536UL); } } catch (OverflowException) { traps++; }
		try { checked { sink = (uint) IdU8 (4294967296UL); } } catch (OverflowException) { traps++; }
		return traps;
	}

	// conv.r.un reads the source as unsigned, and the conv.r4 after it rounds.
	// Both of these values round up to the next power of two.
	public static int test_1_r_un_to_r4_from_u4_and_u8 ()
	{
		return (float) IdU4 (4294967295u) == 4294967296.0f &&
		       (float) IdU8 (18446744073709551615UL) == 18446744073709551616.0f ? 1 : 0;
	}

	/*
	 * The transform folds a conversion whose source it already knows, so the
	 * tests below take the value from a plain local instead of a helper. The
	 * answer is the same either way. What changes is which code computes it.
	 */

	public static int test_1_fold_narrowing_from_i4 ()
	{
		int a = 200;

		return (sbyte) a == -56 && (byte) a == 200 &&
		       (short) a == 200 && (ushort) a == 200 ? 1 : 0;
	}

	public static int test_1_fold_narrowing_from_i8 ()
	{
		long a = 0x1122334455667788L;

		return (sbyte) a == -120 && (byte) a == 136 &&
		       (short) a == 30600 && (ushort) a == 30600 &&
		       (uint) a == 1432778632u && (int) a == 1432778632 ? 1 : 0;
	}

	// Reading an sbyte, a short or a ushort local extends it, and that move
	// folds as well.
	public static int test_1_fold_a_narrow_local ()
	{
		sbyte c = -100;
		short s = -1000;
		ushort u = 60000;

		return c + 1 == -99 && s + 1 == -999 && u + 1 == 60001 ? 1 : 0;
	}

	public static int test_1_fold_ovf_i1_and_u1_from_i4_and_i8 ()
	{
		int a = 100;
		long b = 100L;

		checked {
			return (sbyte) a == 100 && (byte) a == 100 &&
			       (sbyte) b == 100 && (byte) b == 100 ? 1 : 0;
		}
	}

	public static int test_1_fold_ovf_i1_from_u4_and_u8 ()
	{
		uint a = 100u;
		ulong b = 100UL;

		checked {
			return (sbyte) a == 100 && (sbyte) b == 100 ? 1 : 0;
		}
	}

	public static int test_1_fold_ovf_i2_and_u2_from_i4_and_i8 ()
	{
		int a = 1000;
		long b = 1000L;

		checked {
			return (short) a == 1000 && (ushort) a == 1000 &&
			       (short) b == 1000 && (ushort) b == 1000 ? 1 : 0;
		}
	}

	public static int test_1_fold_ovf_i2_from_u4_and_u8 ()
	{
		uint a = 1000u;
		ulong b = 1000UL;

		checked {
			return (short) a == 1000 && (short) b == 1000 ? 1 : 0;
		}
	}

	public static int test_1_fold_ovf_i4_and_u4 ()
	{
		uint a = 7u;
		long b = 7L;
		ulong c = 7UL;

		checked {
			return (int) a == 7 && (int) b == 7 && (uint) b == 7u &&
			       (int) c == 7 ? 1 : 0;
		}
	}

	public static int test_1_fold_ovf_i8_and_u8 ()
	{
		int a = 9;
		long b = 9L;
		ulong c = 9UL;

		checked {
			return (ulong) a == 9UL && (ulong) b == 9UL && (long) c == 9L ? 1 : 0;
		}
	}

	// The fold leaves a constant the destination cannot hold for the opcode to
	// reject at run time, so the trap happens rather than the compile failing.
	public static int test_3_fold_declines_out_of_range_constants ()
	{
		int a = 300;
		long b = -1L;
		ulong c = 0x8000000000000000UL;
		int traps = 0;

		try { checked { sink = (sbyte) a; } } catch (OverflowException) { traps++; }
		try { checked { usink = (ulong) b; } } catch (OverflowException) { traps++; }
		try { checked { sink = (long) c; } } catch (OverflowException) { traps++; }
		return traps;
	}

	// The fold table has no entry for a conversion to floating point, so this
	// one survives and runs.
	public static int test_1_fold_declines_a_float_conversion ()
	{
		int a = 200;

		return (double) a == 200.0 ? 1 : 0;
	}
}
