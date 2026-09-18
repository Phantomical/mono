/* Tests LLVM lowering for System.Runtime.Intrinsics.X86.Sse. */
using System;
using System.Runtime.Intrinsics;
using System.Runtime.Intrinsics.X86;

class Tests
{
	static int failures;

	static unsafe void Check (string what, bool ok)
	{
		if (ok)
			return;
		failures++;
		Console.WriteLine ("FAIL: " + what);
	}

	static unsafe Vector128<float> Load (float e0, float e1, float e2, float e3)
	{
		float* e = stackalloc float[4] { e0, e1, e2, e3 };
		return Sse.LoadVector128 (e);
	}

	static unsafe float[] ToArray (Vector128<float> v)
	{
		float[] r = new float[4];
		fixed (float* p = r)
			Sse.Store (p, v);
		return r;
	}

	static void CheckLanes (string what, Vector128<float> v, float e0, float e1, float e2,
	                        float e3)
	{
		float[] r = ToArray (v);
		Check (what, r [0] == e0 && r [1] == e1 && r [2] == e2 && r [3] == e3);
	}

	// Compare raw bits because bitwise results may represent NaNs.
	static void CheckLane0Bits (string what, Vector128<float> v, uint expected)
	{
		float[] r = ToArray (v);
		Check (what, unchecked ((uint) BitConverter.SingleToInt32Bits (r [0])) == expected
		            && r [1] == 0f && r [2] == 0f && r [3] == 0f);
	}

	// Packed comparisons produce either all-one or all-zero lanes.
	static unsafe void CheckCompare (string what, Vector128<float> v, bool e0, bool e1,
	                                 bool e2, bool e3)
	{
		float[] r = ToArray (v);
		bool[] got = new bool[4];
		for (int i = 0; i < 4; i++) {
			int bits = BitConverter.SingleToInt32Bits (r [i]);
			got [i] = bits == -1;
			Check (what + " lane " + i + " decides ordered/zero",
			      bits == -1 || bits == 0);
		}
		Check (what, got [0] == e0 && got [1] == e1 && got [2] == e2 && got [3] == e3);
	}

	static void CheckThrowsPlatformNotSupported (string what, Action body)
	{
		try {
			body ();
		} catch (PlatformNotSupportedException) {
			return;
		}
		failures++;
		Console.WriteLine ("FAIL: " + what + " did not throw PlatformNotSupportedException");
	}

	static unsafe int Main ()
	{
		Check ("Sse.IsSupported", Sse.IsSupported);

		Vector128<float> a = Load (1f, -2f, 3f, 4f);
		Vector128<float> b = Load (10f, 20f, -30f, 4f);

		CheckLanes ("Add", Sse.Add (a, b), 11f, 18f, -27f, 8f);
		CheckLanes ("Subtract", Sse.Subtract (a, b), -9f, -22f, 33f, 0f);
		CheckLanes ("Multiply", Sse.Multiply (a, b), 10f, -40f, -90f, 16f);
		CheckLanes ("Divide", Sse.Divide (a, b), 0.1f, -0.1f, -0.1f, 1f);

		CheckLanes ("AddScalar", Sse.AddScalar (a, b), 11f, -2f, 3f, 4f);
		CheckLanes ("SubtractScalar", Sse.SubtractScalar (a, b), -9f, -2f, 3f, 4f);
		CheckLanes ("MultiplyScalar", Sse.MultiplyScalar (a, b), 10f, -2f, 3f, 4f);
		CheckLanes ("DivideScalar", Sse.DivideScalar (a, b), 0.1f, -2f, 3f, 4f);

		CheckLanes ("Min", Sse.Min (a, b), 1f, -2f, -30f, 4f);
		CheckLanes ("Max", Sse.Max (a, b), 10f, 20f, 3f, 4f);
		CheckLanes ("MinScalar", Sse.MinScalar (a, b), 1f, -2f, 3f, 4f);
		CheckLanes ("MaxScalar", Sse.MaxScalar (a, b), 10f, -2f, 3f, 4f);

		CheckLanes ("Sqrt", Sse.Sqrt (Load (4f, 9f, 16f, 25f)), 2f, 3f, 4f, 5f);

		float recip = ToArray (Sse.Reciprocal (Load (4f, 4f, 4f, 4f))) [0];
		Check ("Reciprocal is approximately 1/4", Math.Abs (recip - 0.25f) < 0.001f);

		CheckLane0Bits ("And", Sse.And (Load (0xFFFFFFFFu.ToSingle (), 0f, 0f, 0f),
		                                Load (0x0000FFFFu.ToSingle (), 0f, 0f, 0f)),
		               0x0000FFFFu);
		CheckLane0Bits ("Or", Sse.Or (Load (0x0000FFFFu.ToSingle (), 0f, 0f, 0f),
		                              Load (0xFFFF0000u.ToSingle (), 0f, 0f, 0f)),
		               0xFFFFFFFFu);
		CheckLane0Bits ("Xor", Sse.Xor (Load (0xFF00FF00u.ToSingle (), 0f, 0f, 0f),
		                                Load (0xFFFFFFFFu.ToSingle (), 0f, 0f, 0f)),
		               0x00FF00FFu);
		CheckLane0Bits ("AndNot", Sse.AndNot (Load (0x0000FFFFu.ToSingle (), 0f, 0f, 0f),
		                                      Load (0xFFFFFFFFu.ToSingle (), 0f, 0f, 0f)),
		               0xFFFF0000u);

		CheckCompare ("CompareEqual", Sse.CompareEqual (Load (1f, 2f, 3f, 4f),
		                                                Load (1f, 0f, 3f, 0f)),
		             true, false, true, false);
		CheckCompare ("CompareGreaterThan", Sse.CompareGreaterThan (b, a),
		             true, true, false, false);
		CheckCompare ("CompareGreaterThanOrEqual", Sse.CompareGreaterThanOrEqual (a, a),
		             true, true, true, true);
		CheckCompare ("CompareLessThan", Sse.CompareLessThan (a, b),
		             true, true, false, false);
		CheckCompare ("CompareLessThanOrEqual", Sse.CompareLessThanOrEqual (a, a),
		             true, true, true, true);
		CheckCompare ("CompareNotEqual", Sse.CompareNotEqual (a, b),
		             true, true, true, false);

		CheckLanes ("SetZeroVector128", Sse.SetZeroVector128 (), 0f, 0f, 0f, 0f);

		float[] roundtrip = { 5f, 6f, 7f, 8f };
		fixed (float* p = roundtrip) {
			CheckLanes ("LoadVector128/Store", Sse.Add (Sse.LoadVector128 (p),
			                                            Sse.SetZeroVector128 ()),
			           5f, 6f, 7f, 8f);
		}

		// Unlowered ISA classes use their PlatformNotSupported implementations.
		Check ("Sse2.IsSupported", !Sse2.IsSupported);
		Check ("Avx.IsSupported", !Avx.IsSupported);
		Check ("Avx2.IsSupported", !Avx2.IsSupported);
		Check ("Popcnt.IsSupported", !Popcnt.IsSupported);
		Check ("Bmi1.IsSupported", !Bmi1.IsSupported);
		Check ("Aes.IsSupported", !Aes.IsSupported);

		CheckThrowsPlatformNotSupported ("Sse2.Add",
			() => Sse2.Add (default (Vector128<int>), default (Vector128<int>)));
		CheckThrowsPlatformNotSupported ("Avx2.Add",
			() => Avx2.Add (default (Vector128<int>), default (Vector128<int>)));
		CheckThrowsPlatformNotSupported ("Popcnt.PopCount",
			() => Popcnt.PopCount (0u));
		CheckThrowsPlatformNotSupported ("Bmi1.TrailingZeroCount",
			() => Bmi1.TrailingZeroCount (0u));

		if (failures == 0)
			Console.WriteLine ("OK");
		return failures;
	}
}

static class SingleBits
{
	public static unsafe float ToSingle (this uint bits)
	{
		return *(float *) &bits;
	}
}
