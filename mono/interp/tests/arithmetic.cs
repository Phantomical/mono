// Integer and floating point opcodes, one method per behaviour.
//
// A method named test_<n>_<what> is a test, and it passes when it returns <n>.
// Locals rather than constants on both sides of an operator, so the transform
// cannot fold the answer and leave the opcode untested.

using System;

public class Arithmetic {

	static int Id (int x) { return x; }
	static long IdL (long x) { return x; }
	static double IdD (double x) { return x; }

	public static int test_5_add_i4 ()
	{
		int a = Id (2), b = Id (3);
		return a + b;
	}

	public static int test_2_sub_i4 ()
	{
		int a = Id (5), b = Id (3);
		return a - b;
	}

	public static int test_12_mul_i4 ()
	{
		int a = Id (3), b = Id (4);
		return a * b;
	}

	public static int test_3_div_i4 ()
	{
		int a = Id (13), b = Id (4);
		return a / b;
	}

	public static int test_1_rem_i4 ()
	{
		int a = Id (13), b = Id (4);
		return a % b;
	}

	public static int test_7_neg_i4 ()
	{
		int a = Id (-7);
		return -a;
	}

	public static int test_6_and_or_xor_i4 ()
	{
		// (a & b) ^ (a | b) is a ^ b, which is 0x3c here.
		int a = Id (0x0f), b = Id (0x33);
		return (a & b) ^ (a | b) ^ 0x3a;
	}

	public static int test_8_shifts_i4 ()
	{
		int a = Id (1);
		return (a << 4) >> 1;
	}

	public static int test_1_shr_un_i4 ()
	{
		uint a = (uint) Id (-1);
		return (int) (a >> 31);
	}

	public static int test_9_add_i8 ()
	{
		long a = IdL (4000000000L), b = IdL (-3999999991L);
		return (int) (a + b);
	}

	public static int test_4_div_i8 ()
	{
		long a = IdL (-17), b = IdL (-4);
		return (int) (a / b);
	}

	public static int test_3_add_r8 ()
	{
		double a = IdD (1.5), b = IdD (1.5);
		return (int) (a + b);
	}

	public static int test_2_div_r8 ()
	{
		double a = IdD (5.0), b = IdD (2.0);
		return (int) (a / b);
	}

	public static int test_1_r4_keeps_its_width ()
	{
		// The interpreter carries float and double in separate stack types, so a
		// value that survives a round trip through R4 has to come back narrowed.
		float narrowed = (float) IdD (0.1);
		return (double) narrowed == 0.1 ? 0 : 1;
	}

	public static int test_1_conv_ovf_i4_traps ()
	{
		try {
			checked {
				long a = IdL (0x7fffffffL + 1);
				return (int) a;
			}
		} catch (OverflowException) {
			return 1;
		}
	}

	public static int test_1_div_by_zero_throws ()
	{
		try {
			int a = Id (1), b = Id (0);
			return a / b;
		} catch (DivideByZeroException) {
			return 1;
		}
	}
}
