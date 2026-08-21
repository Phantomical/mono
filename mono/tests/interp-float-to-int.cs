using System;
using System.Runtime.CompilerServices;

// An unchecked conversion from a float to an integer, where the value does not
// fit or is a NaN. ECMA-335 leaves the result unspecified. This runtime answers
// with the result the amd64 conversion instruction gives, the integer
// indefinite value. constrained_float_to_int () keeps the compiled tier on that
// instruction, and the interpreter's C cast compiles to it as well. So each
// case below is here because both engines have to answer alike, and the arm
// decides which of them runs.
//
// A narrow destination converts into an int32 first and keeps the low bits of
// that. It is why (byte) -1.0 is 255 and (byte) 1e30 is 0.
class Test {
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ToI4 (double d) { unchecked { return (int) d; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static uint ToU4 (double d) { unchecked { return (uint) d; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long ToI8 (double d) { unchecked { return (long) d; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static ulong ToU8 (double d) { unchecked { return (ulong) d; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static sbyte ToI1 (double d) { unchecked { return (sbyte) d; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static byte ToU1 (double d) { unchecked { return (byte) d; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static short ToI2 (double d) { unchecked { return (short) d; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static ushort ToU2 (double d) { unchecked { return (ushort) d; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SingleToI4 (float f) { unchecked { return (int) f; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long SingleToI8 (float f) { unchecked { return (long) f; } }

	static int failures;

	static void Expect (string what, long got, long want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAILED: " + what + " gave " + got + ", wanted " + want);
		failures++;
	}

	static void ExpectU (string what, ulong got, ulong want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAILED: " + what + " gave " + got + ", wanted " + want);
		failures++;
	}

	public static int Main ()
	{
		// cvttsd2si into a 32 bit register.
		Expect ("(int)1e30", ToI4 (1e30), int.MinValue);
		Expect ("(int)-1e30", ToI4 (-1e30), int.MinValue);
		Expect ("(int)NaN", ToI4 (double.NaN), int.MinValue);

		// An unsigned int32 destination converts into an int64 and keeps the
		// low half, so -1.0 comes back as every bit set.
		Expect ("(uint)1e30", ToU4 (1e30), 0);
		Expect ("(uint)-1.0", ToU4 (-1.0), uint.MaxValue);
		Expect ("(uint)NaN", ToU4 (double.NaN), 0);

		Expect ("(long)1e30", ToI8 (1e30), long.MinValue);
		Expect ("(long)-1e30", ToI8 (-1e30), long.MinValue);
		Expect ("(long)NaN", ToI8 (double.NaN), long.MinValue);

		// An unsigned int64 destination has no one instruction. Both engines
		// send a value below 2^63 to cvttsd2si and the rest through a
		// subtraction. A NaN fails that test, so it goes down the second path.
		ExpectU ("(ulong)1e30", ToU8 (1e30), 0);
		ExpectU ("(ulong)-1.0", ToU8 (-1.0), ulong.MaxValue);
		ExpectU ("(ulong)NaN", ToU8 (double.NaN), 0);

		Expect ("(sbyte)1e30", ToI1 (1e30), 0);
		Expect ("(sbyte)-1e30", ToI1 (-1e30), 0);
		Expect ("(sbyte)NaN", ToI1 (double.NaN), 0);
		Expect ("(byte)1e30", ToU1 (1e30), 0);
		Expect ("(byte)-1.0", ToU1 (-1.0), 255);
		Expect ("(byte)NaN", ToU1 (double.NaN), 0);
		Expect ("(short)1e30", ToI2 (1e30), 0);
		Expect ("(short)-1e30", ToI2 (-1e30), 0);
		Expect ("(short)NaN", ToI2 (double.NaN), 0);
		Expect ("(ushort)1e30", ToU2 (1e30), 0);
		Expect ("(ushort)-1.0", ToU2 (-1.0), 65535);
		Expect ("(ushort)NaN", ToU2 (double.NaN), 0);

		// cvttss2si, reached from a float32 source.
		Expect ("(int)float.PositiveInfinity", SingleToI4 (float.PositiveInfinity),
			int.MinValue);
		Expect ("(int)float.NegativeInfinity", SingleToI4 (float.NegativeInfinity),
			int.MinValue);
		Expect ("(int)float.NaN", SingleToI4 (float.NaN), int.MinValue);
		Expect ("(long)float.PositiveInfinity", SingleToI8 (float.PositiveInfinity),
			long.MinValue);
		Expect ("(long)float.NaN", SingleToI8 (float.NaN), long.MinValue);

		return failures;
	}
}
