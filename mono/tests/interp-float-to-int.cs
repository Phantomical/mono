using System;
using System.Runtime.CompilerServices;

// An unchecked conversion from a float to an integer, where the value does not
// fit. ECMA-335 leaves the result unspecified, and float_to_int () settles which
// answer this runtime gives: saturate, because every path has to reach the same
// one. Two engines are two more paths.
//
// The interpreter casts in C instead, and gets whatever the hardware conversion
// leaves behind for an out-of-range value. So a method answers one way for its
// first calls and another once it is promoted.
class Test {
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ToI4 (double d) { unchecked { return (int) d; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static uint ToU4 (double d) { unchecked { return (uint) d; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long ToI8 (double d) { unchecked { return (long) d; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SingleToI4 (float f) { unchecked { return (int) f; } }

	static int failures;

	static void Expect (string what, long got, long want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAILED: " + what + " gave " + got + ", wanted " + want);
		failures++;
	}

	public static int Main ()
	{
		Expect ("(int)1e30", ToI4 (1e30), int.MaxValue);
		Expect ("(int)-1e30", ToI4 (-1e30), int.MinValue);
		Expect ("(int)NaN", ToI4 (double.NaN), 0);
		Expect ("(uint)1e30", ToU4 (1e30), uint.MaxValue);
		Expect ("(uint)-1.0", ToU4 (-1.0), 0);
		Expect ("(long)1e30", ToI8 (1e30), long.MaxValue);
		Expect ("(int)float.PositiveInfinity", SingleToI4 (float.PositiveInfinity),
			int.MaxValue);
		Expect ("(int)float.NegativeInfinity", SingleToI4 (float.NegativeInfinity),
			int.MinValue);

		return failures;
	}
}
