// The conv.* opcodes over a float source, in the three places conversions.cs
// does not go.
//
// conv.r4 over a value that is already an r4 emits no instruction at all.
// conv.u8 under 2^63 takes the direct arm of mono_rconv_u8 () and
// mono_fconv_u8 (). conversions.cs reaches only the correction arm above 2^63.
// conv.i4 of an in-range source can give int.MinValue, which is also what x86
// gives for a conversion it refuses.
//
// A method named test_<n>_<what> is a test, and it passes when it returns <n>.
// Every operand comes through a NoInlining helper, so the transform cannot fold
// the conversion away and leave the opcode untested.

using System;
using System.Runtime.CompilerServices;

[NoOpt]
public class ConvSources {

	[MethodImpl (MethodImplOptions.NoInlining)] static float IdR4 (float x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double IdR8 (double x) { return x; }

	// conv.r4 over an r4 source. The transform emits nothing for this arm, so
	// each test below asks only that the value arrives unchanged.

	public static int test_1_r4_from_r4_keeps_the_value ()
	{
		return (float) IdR4 (0.1f) == 0.1f &&
		       (float) IdR4 (-16777216f) == -16777216f &&
		       (float) IdR4 (float.Epsilon) == float.Epsilon ? 1 : 0;
	}

	// A zero keeps its sign only in the sign bit, and a division reads that bit
	// back.
	public static int test_1_r4_from_r4_keeps_negative_zero ()
	{
		float z = (float) IdR4 (-0.0f);

		return z == 0.0f && 1.0f / z == float.NegativeInfinity ? 1 : 0;
	}

	// A re-encoding of the value damages a NaN or an infinity first.
	public static int test_1_r4_from_r4_keeps_infinity_and_nan ()
	{
		return (float) IdR4 (float.PositiveInfinity) == float.PositiveInfinity &&
		       float.IsNaN ((float) IdR4 (float.NaN)) ? 1 : 0;
	}

	// x86 gives int.MinValue for a conversion it refuses, so the low end alone
	// passes even when the conversion is wrong. The high end is what separates
	// the two. 2^31 - 128 is the largest float an int holds.
	public static int test_1_i4_from_r4_at_the_ends_of_the_range ()
	{
		return (int) IdR4 (-2147483648f) == int.MinValue &&
		       (int) IdR4 (2147483520f) == 2147483520 ? 1 : 0;
	}

	/*
	 * conv.u8 over a float goes through mono_rconv_u8 () and mono_fconv_u8 (),
	 * which split on 2^63. A source under the limit converts directly. Above it
	 * the helper subtracts 2^63, converts, and puts the top bit back, which is
	 * the arm conversions.cs takes.
	 */

	// 2^63 - 2^39 is the largest whole number under 2^63 that a float holds.
	public static int test_1_u8_from_r4_below_two63 ()
	{
		return (ulong) IdR4 (9223371487098961920f) == 9223371487098961920UL ? 1 : 0;
	}

	// 2^63 - 2^10 is the largest whole number under 2^63 that a double holds.
	public static int test_1_u8_from_r8_below_two63 ()
	{
		return (ulong) IdR8 (9223372036854774784.0) == 9223372036854774784UL ? 1 : 0;
	}
}
