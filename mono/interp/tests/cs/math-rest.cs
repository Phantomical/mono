// The division and remainder opcodes that arithmetic.cs leaves alone:
// MINT_DIV_UN_I8, MINT_REM_I8, MINT_REM_UN_I4 and MINT_REM_UN_I8.  Each one gets
// its in-range cases, its sign edges, and the exceptions it throws.
//
// A method named test_<n>_<what> is a test, and it passes when it returns <n>.
// A test that makes more than one check returns the number of checks that hold,
// so a failure says how many of them were good.
//
// Operands come through NoInlining identity helpers.  A literal operand lets the
// transform fold the operation away, and then the opcode never runs.

using System;
using System.Runtime.CompilerServices;

public class MathRest {

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int MrI (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static uint MrU (uint x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long MrL (long x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static ulong MrUL (ulong x) { return x; }

	static int Ok (bool held) { return held ? 1 : 0; }

	//
	// div.un on 64-bit operands.  MINT_DIV_UN_I8.
	//

	public static int test_14_div_un_i8_in_range ()
	{
		ulong a = MrUL (100), b = MrUL (7);
		return (int) (a / b);
	}

	public static int test_2_div_un_i8_max_value ()
	{
		ulong a = MrUL (UInt64.MaxValue), b = MrUL (3);
		return Ok (a / b == 6148914691236517205UL) + Ok (a / a == 1);
	}

	// The same bits under div give Int64.MinValue / 2, which is negative.
	public static int test_1_div_un_i8_high_bit_dividend ()
	{
		ulong a = MrUL (0x8000000000000000UL), b = MrUL (2);
		return Ok (a / b == 0x4000000000000000UL);
	}

	// ECMA-335 III.3.32: +5 div.un -3 is 0.
	public static int test_1_div_un_i8_divisor_has_high_bit ()
	{
		ulong a = MrUL (5), b = MrUL (0x8000000000000000UL);
		return Ok (a / b == 0);
	}

	// ECMA-335 III.3.32: -5 div.un +3 is 0x55555553 in 32 bits.  The 64-bit form
	// has the same shape.
	public static int test_1_div_un_i8_negative_bits_are_unsigned ()
	{
		ulong a = MrUL ((ulong) MrL (-5)), b = MrUL (3);
		return Ok (a / b == 6148914691236517203UL);
	}

	// div.un has no overflow case, so the operands that make div throw give a
	// quotient here.
	public static int test_1_div_un_i8_min_bits_by_all_ones ()
	{
		ulong a = MrUL (0x8000000000000000UL), b = MrUL (0xffffffffffffffffUL);
		return Ok (a / b == 0);
	}

	public static int test_1_div_un_i8_by_zero_throws ()
	{
		ulong a = MrUL (10), b = MrUL (0);
		try {
			return (int) (a / b);
		} catch (DivideByZeroException) {
			return 1;
		}
	}

	//
	// rem on 64-bit operands.  MINT_REM_I8.
	//

	// ECMA-335 III.3.55: sign(result) = sign(value1).
	public static int test_4_rem_i8_sign_follows_the_dividend ()
	{
		long a = MrL (17), b = MrL (5), c = MrL (-17), d = MrL (-5);
		return Ok (a % b == 2)
		     + Ok (c % b == -2)
		     + Ok (a % d == 2)
		     + Ok (c % d == -2);
	}

	public static int test_1_rem_i8_wider_than_32_bits ()
	{
		long a = MrL (5000000000L), b = MrL (3000000000L);
		return Ok (a % b == 2000000000L);
	}

	public static int test_0_rem_i8_exact_division ()
	{
		long a = MrL (-20), b = MrL (5);
		return (int) (a % b);
	}

	// A -1 divisor with any other dividend is the near miss of the overflow guard.
	public static int test_2_rem_i8_by_one_and_minus_one ()
	{
		long a = MrL (-9007199254740993L), one = MrL (1), minus = MrL (-1);
		return Ok (a % one == 0) + Ok (a % minus == 0);
	}

	public static int test_1_rem_i8_max_value ()
	{
		long a = MrL (Int64.MaxValue), b = MrL (2);
		return Ok (a % b == 1);
	}

	public static int test_1_rem_i8_min_value ()
	{
		long a = MrL (Int64.MinValue), b = MrL (3);
		return Ok (a % b == -2);
	}

	public static int test_2_rem_i8_matches_its_quotient ()
	{
		long a = MrL (-1234567890123L), b = MrL (7777);
		return Ok (a / b == -158746031L) + Ok (a % b == -7036L);
	}

	// The quotient of Int64.MinValue and -1 does not fit, so rem reports the same
	// overflow that div does.
	public static int test_1_rem_i8_min_value_by_minus_one_throws ()
	{
		long a = MrL (Int64.MinValue), b = MrL (-1);
		try {
			return (int) (a % b);
		} catch (OverflowException) {
			return 1;
		}
	}

	// This dividend overflows against a -1 divisor.  A zero divisor comes first.
	public static int test_1_rem_i8_by_zero_throws ()
	{
		long a = MrL (Int64.MinValue), b = MrL (0);
		try {
			return (int) (a % b);
		} catch (DivideByZeroException) {
			return 1;
		}
	}

	//
	// rem.un on 32-bit operands.  MINT_REM_UN_I4.
	//

	public static int test_5_rem_un_i4_max_dividend ()
	{
		uint a = MrU (UInt32.MaxValue), b = MrU (10);
		return (int) (a % b);
	}

	public static int test_2_rem_un_i4_high_bit_dividend ()
	{
		uint a = MrU (0x80000000U), b = MrU (3);
		return (int) (a % b);
	}

	public static int test_2_rem_un_i4_gives_zero ()
	{
		uint a = MrU (0xdeadbeefU), one = MrU (1), max = MrU (UInt32.MaxValue);
		return Ok (a % one == 0) + Ok (max % max == 0);
	}

	// The four examples of ECMA-335 III.3.56.
	public static int test_4_rem_un_i4_ecma_examples ()
	{
		uint five = MrU (5), three = MrU (3);
		uint minus_five = MrU ((uint) MrI (-5)), minus_three = MrU ((uint) MrI (-3));
		return Ok (five % three == 2)
		     + Ok (five % minus_three == 5)
		     + Ok (minus_five % three == 2)
		     + Ok (minus_five % minus_three == 0xfffffffbU);
	}

	// The same bits under rem give -2, so the sign of the dividend is what tells
	// the two opcodes apart.
	public static int test_2_rem_un_i4_differs_from_signed ()
	{
		int signed_a = MrI (-100), signed_b = MrI (7);
		uint a = MrU ((uint) signed_a), b = MrU ((uint) signed_b);
		return Ok (signed_a % signed_b == -2) + Ok (a % b == 2);
	}

	// rem.un has no overflow case, so the operands that make rem throw give a
	// remainder here.
	public static int test_1_rem_un_i4_min_bits_by_all_ones ()
	{
		uint a = MrU (0x80000000U), b = MrU (0xffffffffU);
		return Ok (a % b == 0x80000000U);
	}

	public static int test_1_rem_un_i4_by_zero_throws ()
	{
		uint a = MrU (UInt32.MaxValue), b = MrU (0);
		try {
			return (int) (a % b);
		} catch (DivideByZeroException) {
			return 1;
		}
	}

	//
	// rem.un on 64-bit operands.  MINT_REM_UN_I8.
	//

	public static int test_5_rem_un_i8_max_dividend ()
	{
		ulong a = MrUL (UInt64.MaxValue), b = MrUL (10);
		return (int) (a % b);
	}

	public static int test_2_rem_un_i8_high_bit_dividend ()
	{
		ulong a = MrUL (0x8000000000000000UL), b = MrUL (3);
		return (int) (a % b);
	}

	public static int test_2_rem_un_i8_gives_zero ()
	{
		ulong a = MrUL (0xdeadbeefcafef00dUL), one = MrUL (1), max = MrUL (UInt64.MaxValue);
		return Ok (a % one == 0) + Ok (max % max == 0);
	}

	// The four examples of ECMA-335 III.3.56 at 64 bits.
	public static int test_4_rem_un_i8_ecma_examples ()
	{
		ulong five = MrUL (5), three = MrUL (3);
		ulong minus_five = MrUL ((ulong) MrL (-5)), minus_three = MrUL ((ulong) MrL (-3));
		return Ok (five % three == 2)
		     + Ok (five % minus_three == 5)
		     + Ok (minus_five % three == 2)
		     + Ok (minus_five % minus_three == 0xfffffffffffffffbUL);
	}

	public static int test_2_rem_un_i8_differs_from_signed ()
	{
		long signed_a = MrL (-100), signed_b = MrL (7);
		ulong a = MrUL ((ulong) signed_a), b = MrUL ((ulong) signed_b);
		return Ok (signed_a % signed_b == -2) + Ok (a % b == 0);
	}

	public static int test_1_rem_un_i8_min_bits_by_all_ones ()
	{
		ulong a = MrUL (0x8000000000000000UL), b = MrUL (0xffffffffffffffffUL);
		return Ok (a % b == 0x8000000000000000UL);
	}

	public static int test_2_rem_un_i8_matches_its_quotient ()
	{
		ulong a = MrUL (0xfedcba9876543210UL), b = MrUL (0x0badc0deUL);
		return Ok (a / b == 93728124195UL) + Ok (a % b == 178179510UL);
	}

	public static int test_1_rem_un_i8_by_zero_throws ()
	{
		ulong a = MrUL (UInt64.MaxValue), b = MrUL (0);
		try {
			return (int) (a % b);
		} catch (DivideByZeroException) {
			return 1;
		}
	}

	// A caught DivideByZeroException leaves the two opcodes usable afterwards.
	public static int test_3_division_survives_a_throw ()
	{
		ulong a = MrUL (0xffffffffffffffffUL), zero = MrUL (0), b = MrUL (16);
		int caught = 0;
		try {
			a = a / zero;
		} catch (DivideByZeroException) {
			caught = 1;
		}
		return caught + Ok (a / b == 0x0fffffffffffffffUL) + Ok (a % b == 15);
	}
}
