/* Tests LLVM lowering for System.Runtime.Intrinsics.X86.Sse and Sse2. */
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

	static unsafe Vector128<int> LoadI32 (int e0, int e1, int e2, int e3)
	{
		int* e = stackalloc int[4] { e0, e1, e2, e3 };
		return Sse2.LoadVector128 (e);
	}

	static unsafe int[] ToArray (Vector128<int> v)
	{
		int[] r = new int[4];
		fixed (int* p = r)
			Sse2.Store (p, v);
		return r;
	}

	static void CheckLanesI32 (string what, Vector128<int> v, int e0, int e1, int e2, int e3)
	{
		int[] r = ToArray (v);
		Check (what, r [0] == e0 && r [1] == e1 && r [2] == e2 && r [3] == e3);
	}

	static unsafe Vector128<double> LoadF64 (double e0, double e1)
	{
		double* e = stackalloc double[2] { e0, e1 };
		return Sse2.LoadVector128 (e);
	}

	static unsafe double[] ToArray (Vector128<double> v)
	{
		double[] r = new double[2];
		fixed (double* p = r)
			Sse2.Store (p, v);
		return r;
	}

	static void CheckLanesF64 (string what, Vector128<double> v, double e0, double e1)
	{
		double[] r = ToArray (v);
		Check (what, r [0] == e0 && r [1] == e1);
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

		Check ("Sse2.IsSupported", Sse2.IsSupported);

		Vector128<int> ia = LoadI32 (1, -2, 3, 4);
		Vector128<int> ib = LoadI32 (10, 20, -30, 4);

		CheckLanesI32 ("Sse2.Add(int)", Sse2.Add (ia, ib), 11, 18, -27, 8);
		CheckLanesI32 ("Sse2.Subtract(int)", Sse2.Subtract (ia, ib), -9, -22, 33, 0);
		CheckLanesI32 ("Sse2.CompareEqual(int)", Sse2.CompareEqual (ia, LoadI32 (1, 0, 3, 0)),
		              -1, 0, -1, 0);
		CheckLanesI32 ("Sse2.CompareGreaterThan(int)", Sse2.CompareGreaterThan (ib, ia),
		              -1, -1, 0, 0);
		CheckLanesI32 ("Sse2.CompareLessThan(int)", Sse2.CompareLessThan (ia, ib), -1, -1, 0, 0);
		CheckLanesI32 ("Sse2.And(int)", Sse2.And (LoadI32 (0x0F, 0, 0, 0), LoadI32 (0x03, 0, 0, 0)),
		              0x03, 0, 0, 0);
		CheckLanesI32 ("Sse2.Or(int)", Sse2.Or (LoadI32 (0x0F, 0, 0, 0), LoadI32 (0x30, 0, 0, 0)),
		              0x3F, 0, 0, 0);
		CheckLanesI32 ("Sse2.Xor(int)", Sse2.Xor (ia, ia), 0, 0, 0, 0);
		CheckLanesI32 ("Sse2.AndNot(int)",
		              Sse2.AndNot (LoadI32 (0x0F, 0, 0, 0), LoadI32 (-1, 0, 0, 0)), -16, 0, 0, 0);
		CheckLanesI32 ("Sse2.SetZeroVector128<int>", Sse2.SetZeroVector128<int> (), 0, 0, 0, 0);

		Vector128<double> da = LoadF64 (4.0, 9.0);
		Vector128<double> db = LoadF64 (2.0, 3.0);

		CheckLanesF64 ("Sse2.Add(double)", Sse2.Add (da, db), 6.0, 12.0);
		CheckLanesF64 ("Sse2.Subtract(double)", Sse2.Subtract (da, db), 2.0, 6.0);
		CheckLanesF64 ("Sse2.Multiply(double)", Sse2.Multiply (da, db), 8.0, 27.0);
		CheckLanesF64 ("Sse2.Divide(double)", Sse2.Divide (da, db), 2.0, 3.0);
		CheckLanesF64 ("Sse2.Sqrt(double)", Sse2.Sqrt (da), 2.0, 3.0);
		CheckLanesF64 ("Sse2.Min(double)", Sse2.Min (da, db), 2.0, 3.0);
		CheckLanesF64 ("Sse2.Max(double)", Sse2.Max (da, db), 4.0, 9.0);

		// Sse2 supports integer min/max for unsigned bytes and signed shorts.
		unsafe {
			byte* be = stackalloc byte[16] { 5, 200, 3, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
			byte* bf = stackalloc byte[16] { 10, 100, 3, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
			Vector128<byte> vbe = Sse2.LoadVector128 (be);
			Vector128<byte> vbf = Sse2.LoadVector128 (bf);
			byte[] br = new byte[16];
			fixed (byte* p = br) Sse2.Store (p, Sse2.Min (vbe, vbf));
			Check ("Sse2.Min(byte)", br [0] == 5 && br [1] == 100 && br [2] == 3 && br [3] == 4);
			fixed (byte* p = br) Sse2.Store (p, Sse2.Max (vbe, vbf));
			Check ("Sse2.Max(byte)", br [0] == 10 && br [1] == 200 && br [2] == 3 && br [3] == 9);
		}

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
