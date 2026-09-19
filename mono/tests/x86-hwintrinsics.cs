/* Tests LLVM lowering for System.Runtime.Intrinsics.X86.*. */
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

	static unsafe Vector128<sbyte> LoadI8 (params sbyte[] e)
	{
		fixed (sbyte* p = e)
			return Sse2.LoadVector128 (p);
	}

	static unsafe sbyte[] ToArrayI8 (Vector128<sbyte> v)
	{
		sbyte[] r = new sbyte[16];
		fixed (sbyte* p = r)
			Sse2.Store (p, v);
		return r;
	}

	static unsafe byte[] ToArrayU8 (Vector128<byte> v)
	{
		byte[] r = new byte[16];
		fixed (byte* p = r)
			Sse2.Store (p, v);
		return r;
	}

	static void CheckArrayI8 (string what, sbyte[] got, params sbyte[] expected)
	{
		bool ok = got.Length == expected.Length;
		for (int i = 0; ok && i < got.Length; i++)
			ok &= got [i] == expected [i];
		Check (what, ok);
	}

	static void CheckArrayU8 (string what, byte[] got, params byte[] expected)
	{
		bool ok = got.Length == expected.Length;
		for (int i = 0; ok && i < got.Length; i++)
			ok &= got [i] == expected [i];
		Check (what, ok);
	}

	static unsafe Vector128<short> LoadI16 (params short[] e)
	{
		fixed (short* p = e)
			return Sse2.LoadVector128 (p);
	}

	static unsafe short[] ToArrayI16 (Vector128<short> v)
	{
		short[] r = new short[8];
		fixed (short* p = r)
			Sse2.Store (p, v);
		return r;
	}

	static void CheckArrayI16 (string what, short[] got, params short[] expected)
	{
		bool ok = got.Length == expected.Length;
		for (int i = 0; ok && i < got.Length; i++)
			ok &= got [i] == expected [i];
		Check (what, ok);
	}

	static unsafe Vector128<byte> LoadU8 (params byte[] e)
	{
		fixed (byte* p = e)
			return Sse2.LoadVector128 (p);
	}

	static unsafe Vector128<ushort> LoadU16 (params ushort[] e)
	{
		fixed (ushort* p = e)
			return Sse2.LoadVector128 (p);
	}

	static unsafe ushort[] ToArrayU16 (Vector128<ushort> v)
	{
		ushort[] r = new ushort[8];
		fixed (ushort* p = r)
			Sse2.Store (p, v);
		return r;
	}

	static void CheckArrayU16 (string what, ushort[] got, params ushort[] expected)
	{
		bool ok = got.Length == expected.Length;
		for (int i = 0; ok && i < got.Length; i++)
			ok &= got [i] == expected [i];
		Check (what, ok);
	}

	static unsafe Vector128<uint> LoadU32 (uint e0, uint e1, uint e2, uint e3)
	{
		uint* e = stackalloc uint[4] { e0, e1, e2, e3 };
		return Sse2.LoadVector128 (e);
	}

	static unsafe uint[] ToArrayU32 (Vector128<uint> v)
	{
		uint[] r = new uint[4];
		fixed (uint* p = r)
			Sse2.Store (p, v);
		return r;
	}

	static void CheckLanesU32 (string what, Vector128<uint> v, uint e0, uint e1, uint e2, uint e3)
	{
		uint[] r = ToArrayU32 (v);
		Check (what, r [0] == e0 && r [1] == e1 && r [2] == e2 && r [3] == e3);
	}

	static unsafe Vector128<long> LoadI64 (long e0, long e1)
	{
		long* e = stackalloc long[2] { e0, e1 };
		return Sse2.LoadVector128 (e);
	}

	static unsafe long[] ToArrayI64 (Vector128<long> v)
	{
		long[] r = new long[2];
		fixed (long* p = r)
			Sse2.Store (p, v);
		return r;
	}

	static void CheckLanesI64 (string what, Vector128<long> v, long e0, long e1)
	{
		long[] r = ToArrayI64 (v);
		Check (what, r [0] == e0 && r [1] == e1);
	}

	static unsafe Vector256<float> LoadF256 (float e0, float e1, float e2, float e3, float e4,
	                                         float e5, float e6, float e7)
	{
		float* e = stackalloc float[8] { e0, e1, e2, e3, e4, e5, e6, e7 };
		return Avx.LoadVector256 (e);
	}

	static unsafe float[] ToArrayF256 (Vector256<float> v)
	{
		float[] r = new float[8];
		fixed (float* p = r)
			Avx.Store (p, v);
		return r;
	}

	static void CheckLanesF256 (string what, Vector256<float> v, float e0, float e1, float e2,
	                            float e3, float e4, float e5, float e6, float e7)
	{
		float[] r = ToArrayF256 (v);
		Check (what, r [0] == e0 && r [1] == e1 && r [2] == e2 && r [3] == e3 && r [4] == e4
		            && r [5] == e5 && r [6] == e6 && r [7] == e7);
	}

	static unsafe Vector256<double> LoadD256 (double e0, double e1, double e2, double e3)
	{
		double* e = stackalloc double[4] { e0, e1, e2, e3 };
		return Avx.LoadVector256 (e);
	}

	static unsafe double[] ToArrayD256 (Vector256<double> v)
	{
		double[] r = new double[4];
		fixed (double* p = r)
			Avx.Store (p, v);
		return r;
	}

	static void CheckLanesD256 (string what, Vector256<double> v, double e0, double e1, double e2,
	                            double e3)
	{
		double[] r = ToArrayD256 (v);
		Check (what, r [0] == e0 && r [1] == e1 && r [2] == e2 && r [3] == e3);
	}

	static unsafe Vector256<int> LoadI256 (int e0, int e1, int e2, int e3, int e4, int e5, int e6,
	                                       int e7)
	{
		int* e = stackalloc int[8] { e0, e1, e2, e3, e4, e5, e6, e7 };
		return Avx.LoadVector256 (e);
	}

	static unsafe int[] ToArrayI256 (Vector256<int> v)
	{
		int[] r = new int[8];
		fixed (int* p = r)
			Avx.Store (p, v);
		return r;
	}

	static void CheckLanesI256 (string what, Vector256<int> v, int e0, int e1, int e2, int e3,
	                            int e4, int e5, int e6, int e7)
	{
		int[] r = ToArrayI256 (v);
		Check (what, r [0] == e0 && r [1] == e1 && r [2] == e2 && r [3] == e3 && r [4] == e4
		            && r [5] == e5 && r [6] == e6 && r [7] == e7);
	}

	static unsafe Vector256<uint> LoadU256 (uint e0, uint e1, uint e2, uint e3, uint e4, uint e5,
	                                        uint e6, uint e7)
	{
		uint* e = stackalloc uint[8] { e0, e1, e2, e3, e4, e5, e6, e7 };
		return Avx.LoadVector256 (e);
	}

	static unsafe uint[] ToArrayU256 (Vector256<uint> v)
	{
		uint[] r = new uint[8];
		fixed (uint* p = r)
			Avx.Store (p, v);
		return r;
	}

	static unsafe Vector256<sbyte> LoadI8x256 (params sbyte[] e)
	{
		fixed (sbyte* p = e)
			return Avx.LoadVector256 (p);
	}

	static unsafe sbyte[] ToArrayI8x256 (Vector256<sbyte> v)
	{
		sbyte[] r = new sbyte[32];
		fixed (sbyte* p = r)
			Avx.Store (p, v);
		return r;
	}

	static unsafe Vector256<byte> LoadU8x256 (params byte[] e)
	{
		fixed (byte* p = e)
			return Avx.LoadVector256 (p);
	}

	static unsafe byte[] ToArrayU8x256 (Vector256<byte> v)
	{
		byte[] r = new byte[32];
		fixed (byte* p = r)
			Avx.Store (p, v);
		return r;
	}

	static unsafe Vector256<short> LoadI16x256 (params short[] e)
	{
		fixed (short* p = e)
			return Avx.LoadVector256 (p);
	}

	static unsafe short[] ToArrayI16x256 (Vector256<short> v)
	{
		short[] r = new short[16];
		fixed (short* p = r)
			Avx.Store (p, v);
		return r;
	}

	static unsafe Vector256<ushort> LoadU16x256 (params ushort[] e)
	{
		fixed (ushort* p = e)
			return Avx.LoadVector256 (p);
	}

	static unsafe ushort[] ToArrayU16x256 (Vector256<ushort> v)
	{
		ushort[] r = new ushort[16];
		fixed (ushort* p = r)
			Avx.Store (p, v);
		return r;
	}

	static unsafe Vector256<long> LoadI64x256 (long e0, long e1, long e2, long e3)
	{
		long* e = stackalloc long[4] { e0, e1, e2, e3 };
		return Avx.LoadVector256 (e);
	}

	static unsafe long[] ToArrayI64x256 (Vector256<long> v)
	{
		long[] r = new long[4];
		fixed (long* p = r)
			Avx.Store (p, v);
		return r;
	}

	static unsafe Vector256<ulong> LoadU64x256 (ulong e0, ulong e1, ulong e2, ulong e3)
	{
		ulong* e = stackalloc ulong[4] { e0, e1, e2, e3 };
		return Avx.LoadVector256 (e);
	}

	static unsafe ulong[] ToArrayU64x256 (Vector256<ulong> v)
	{
		ulong[] r = new ulong[4];
		fixed (ulong* p = r)
			Avx.Store (p, v);
		return r;
	}

	static void CheckArrayI8x256 (string what, sbyte[] got, params sbyte[] expected)
	{
		bool ok = got.Length == expected.Length;
		for (int i = 0; ok && i < got.Length; i++)
			ok &= got [i] == expected [i];
		Check (what, ok);
	}

	static void CheckArrayI16x256 (string what, short[] got, params short[] expected)
	{
		bool ok = got.Length == expected.Length;
		for (int i = 0; ok && i < got.Length; i++)
			ok &= got [i] == expected [i];
		Check (what, ok);
	}

	static void CheckArrayU8x256 (string what, byte[] got, params byte[] expected)
	{
		bool ok = got.Length == expected.Length;
		for (int i = 0; ok && i < got.Length; i++)
			ok &= got [i] == expected [i];
		Check (what, ok);
	}

	static void CheckArrayU16x256 (string what, ushort[] got, params ushort[] expected)
	{
		bool ok = got.Length == expected.Length;
		for (int i = 0; ok && i < got.Length; i++)
			ok &= got [i] == expected [i];
		Check (what, ok);
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

		Check ("Sse3.IsSupported", Sse3.IsSupported);

		if (Sse3.IsSupported) {
			CheckLanes ("Sse3.AddSubtract(float)", Sse3.AddSubtract (a, b), -9f, 18f, 33f, 8f);
			CheckLanes ("Sse3.HorizontalAdd(float)", Sse3.HorizontalAdd (a, b), -1f, 7f, 30f,
			           -26f);
			CheckLanes ("Sse3.HorizontalSubtract(float)", Sse3.HorizontalSubtract (a, b),
			           3f, -1f, -10f, -34f);
			CheckLanes ("Sse3.MoveHighAndDuplicate", Sse3.MoveHighAndDuplicate (a),
			           -2f, -2f, 4f, 4f);
			CheckLanes ("Sse3.MoveLowAndDuplicate", Sse3.MoveLowAndDuplicate (a), 1f, 1f, 3f, 3f);

			CheckLanesF64 ("Sse3.AddSubtract(double)", Sse3.AddSubtract (da, db), 2.0, 12.0);
			CheckLanesF64 ("Sse3.HorizontalAdd(double)", Sse3.HorizontalAdd (da, db), 13.0, 5.0);
			CheckLanesF64 ("Sse3.HorizontalSubtract(double)", Sse3.HorizontalSubtract (da, db),
			              -5.0, -1.0);
			CheckLanesF64 ("Sse3.MoveAndDuplicate", Sse3.MoveAndDuplicate (da), 4.0, 4.0);

			unsafe {
				double dup = 7.0;
				CheckLanesF64 ("Sse3.LoadAndDuplicateToVector128",
				              Sse3.LoadAndDuplicateToVector128 (&dup), 7.0, 7.0);
			}

			unsafe {
				int* e = stackalloc int[4] { 11, -22, 33, -44 };
				CheckLanesI32 ("Sse3.LoadDquVector128", Sse3.LoadDquVector128 (e), 11, -22, 33,
				              -44);
			}
		}

		Check ("Ssse3.IsSupported", Ssse3.IsSupported);

		if (Ssse3.IsSupported) {
			CheckArrayU8 ("Ssse3.Abs(sbyte)",
			             ToArrayU8 (Ssse3.Abs (LoadI8 (-1, 2, -3, 4, -128, 127, -5, 0, 0, 0, 0, 0,
			                                           0, 0, 0, 0))),
			             1, 2, 3, 4, 128, 127, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0);

			{
				Vector128<sbyte> ar = LoadI8 (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
				                              15);
				Vector128<sbyte> al = LoadI8 (16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
				                              29, 30, 31);

				CheckArrayI8 ("Ssse3.AlignRight(mask=5)", ToArrayI8 (Ssse3.AlignRight (al, ar, 5)),
				             5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20);
				CheckArrayI8 ("Ssse3.AlignRight(mask=0)", ToArrayI8 (Ssse3.AlignRight (al, ar, 0)),
				             0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
				CheckArrayI8 ("Ssse3.AlignRight(mask=20)",
				             ToArrayI8 (Ssse3.AlignRight (al, ar, 20)),
				             20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 0, 0, 0, 0);
				CheckArrayI8 ("Ssse3.AlignRight(mask=32)",
				             ToArrayI8 (Ssse3.AlignRight (al, ar, 32)),
				             0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
			}

			{
				Vector128<short> sl = LoadI16 (1, 2, 3, 4, 5, 6, 7, 8);
				Vector128<short> sr = LoadI16 (10, 20, 30, 40, 50, 60, 70, 80);

				CheckArrayI16 ("Ssse3.HorizontalAdd(short)",
				              ToArrayI16 (Ssse3.HorizontalAdd (sl, sr)),
				              3, 7, 11, 15, 30, 70, 110, 150);
				CheckArrayI16 ("Ssse3.HorizontalSubtract(short)",
				              ToArrayI16 (Ssse3.HorizontalSubtract (sl, sr)),
				              -1, -1, -1, -1, -10, -10, -10, -10);

				Vector128<short> satAdd = LoadI16 (30000, 30000, 0, 0, 0, 0, 0, 0);
				CheckArrayI16 ("Ssse3.HorizontalAddSaturate",
				              ToArrayI16 (Ssse3.HorizontalAddSaturate (satAdd,
				                                                       LoadI16 (0, 0, 0, 0, 0, 0,
				                                                                0, 0))),
				              32767, 0, 0, 0, 0, 0, 0, 0);

				Vector128<short> satSub = LoadI16 (-30000, 30000, 0, 0, 0, 0, 0, 0);
				CheckArrayI16 ("Ssse3.HorizontalSubtractSaturate",
				              ToArrayI16 (Ssse3.HorizontalSubtractSaturate (satSub,
				                                                           LoadI16 (0, 0, 0, 0,
				                                                                    0, 0, 0, 0))),
				              -32768, 0, 0, 0, 0, 0, 0, 0);
			}

			CheckLanesI32 ("Ssse3.HorizontalAdd(int)",
			              Ssse3.HorizontalAdd (LoadI32 (1, 2, 3, 4), LoadI32 (10, 20, 30, 40)),
			              3, 7, 30, 70);
			CheckLanesI32 ("Ssse3.HorizontalSubtract(int)",
			              Ssse3.HorizontalSubtract (LoadI32 (1, 2, 3, 4), LoadI32 (10, 20, 30,
			                                                                      40)),
			              -1, -1, -10, -10);

			unsafe {
				byte* lp = stackalloc byte[16] { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
				                                 16 };
				sbyte* rp = stackalloc sbyte[16] { 1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 7, -7,
				                                   8, -8 };
				Vector128<byte> ml = Sse2.LoadVector128 (lp);
				Vector128<sbyte> mr = Sse2.LoadVector128 (rp);

				CheckArrayI16 ("Ssse3.MultiplyAddAdjacent",
				              ToArrayI16 (Ssse3.MultiplyAddAdjacent (ml, mr)),
				              -1, -2, -3, -4, -5, -6, -7, -8);
			}

			CheckArrayI16 ("Ssse3.MultiplyHighRoundScale",
			              ToArrayI16 (Ssse3.MultiplyHighRoundScale (
				              LoadI16 (16384, 8192, 4096, 2048, 1024, 512, 256, 128),
				              LoadI16 (16384, 16384, 16384, 16384, 16384, 16384, 16384,
				                      16384))),
			              8192, 4096, 2048, 1024, 512, 256, 128, 64);

			CheckArrayI8 ("Ssse3.Shuffle(sbyte)",
			             ToArrayI8 (Ssse3.Shuffle (
				             LoadI8 (100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
				                    112, 113, 114, 115),
				             LoadI8 (15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, -128))),
			             115, 114, 113, 112, 111, 110, 109, 108, 107, 106, 105, 104, 103, 102,
			             101, 0);

			unsafe {
				byte* vp = stackalloc byte[16] { 200, 201, 202, 203, 204, 205, 206, 207, 208, 209,
				                                 210, 211, 212, 213, 214, 215 };
				byte* mp = stackalloc byte[16] { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
				                                 0x80 };
				CheckArrayU8 ("Ssse3.Shuffle(byte)",
				             ToArrayU8 (Ssse3.Shuffle (Sse2.LoadVector128 (vp),
				                                       Sse2.LoadVector128 (mp))),
				             200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213,
				             214, 0);
			}

			CheckArrayI8 ("Ssse3.Sign(sbyte)",
			             ToArrayI8 (Ssse3.Sign (
				             LoadI8 (5, -5, 3, -3, 7, -7, 9, -9, 0, 0, 0, 0, 0, 0, 0, 0),
				             LoadI8 (1, 1, -1, -1, 0, 0, 2, -2, 0, 0, 0, 0, 0, 0, 0, 0))),
			             5, -5, -3, 3, 0, 0, 9, 9, 0, 0, 0, 0, 0, 0, 0, 0);
			CheckArrayI16 ("Ssse3.Sign(short)",
			              ToArrayI16 (Ssse3.Sign (LoadI16 (5, -5, 3, -3, 7, -7, 9, -9),
			                                     LoadI16 (1, 1, -1, -1, 0, 0, 2, -2))),
			              5, -5, -3, 3, 0, 0, 9, 9);
			CheckLanesI32 ("Ssse3.Sign(int)",
			              Ssse3.Sign (LoadI32 (5, -5, 3, -3), LoadI32 (1, -1, 0, 2)), 5, 5, 0, -3);
		}

		Check ("Sse41.IsSupported", Sse41.IsSupported);

		if (Sse41.IsSupported) {
			CheckArrayI16 ("Sse41.Blend(short)",
			              ToArrayI16 (Sse41.Blend (LoadI16 (1, 2, 3, 4, 5, 6, 7, 8),
			                                       LoadI16 (10, 20, 30, 40, 50, 60, 70, 80),
			                                       0xAA)),
			              1, 20, 3, 40, 5, 60, 7, 80);

			Vector128<float> fa = Load (1f, 2f, 3f, 4f);
			Vector128<float> fb = Load (100f, 200f, 300f, 400f);

			CheckLanes ("Sse41.Blend(float)", Sse41.Blend (fa, fb, 6), 1f, 200f, 300f, 4f);
			CheckLanes ("Sse41.BlendVariable(float)",
			           Sse41.BlendVariable (fa, fb, Load (-1f, 1f, -1f, 1f)), 100f, 2f, 300f, 4f);

			CheckArrayI8 ("Sse41.BlendVariable(sbyte)",
			             ToArrayI8 (Sse41.BlendVariable (
				             LoadI8 (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16),
				             LoadI8 (-1, -2, -3, -4, -5, -6, -7, -8, -9, -10, -11, -12, -13,
				                    -14, -15, -16),
				             LoadI8 (0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1))),
			             1, -2, 3, -4, 5, -6, 7, -8, 9, -10, 11, -12, 13, -14, 15, -16);

			CheckLanes ("Sse41.Ceiling(float)", Sse41.Ceiling (Load (1.2f, -1.2f, 2.7f, -2.7f)),
			           2f, -1f, 3f, -2f);
			CheckLanes ("Sse41.Floor(float)", Sse41.Floor (Load (1.2f, -1.2f, 2.7f, -2.7f)),
			           1f, -2f, 2f, -3f);
			CheckLanesF64 ("Sse41.Ceiling(double)", Sse41.Ceiling (LoadF64 (1.2, -1.2)), 2.0, -1.0);
			CheckLanesF64 ("Sse41.Floor(double)", Sse41.Floor (LoadF64 (2.7, -2.7)), 2.0, -3.0);

			CheckLanes ("Sse41.CeilingScalar(value)",
			           Sse41.CeilingScalar (Load (1.2f, 7f, 8f, 9f)), 2f, 7f, 8f, 9f);
			CheckLanes ("Sse41.FloorScalar(upper, value)",
			           Sse41.FloorScalar (Load (100f, 101f, 102f, 103f), Load (2.7f, 0f, 0f, 0f)),
			           2f, 101f, 102f, 103f);

			Vector128<float> ties = Load (2.5f, -2.5f, 1.5f, -1.5f);

			CheckLanes ("Sse41.RoundToNearestInteger", Sse41.RoundToNearestInteger (ties),
			           2f, -2f, 2f, -2f);
			CheckLanes ("Sse41.RoundToZero", Sse41.RoundToZero (ties), 2f, -2f, 1f, -1f);

			CheckLanesI64 ("Sse41.CompareEqual(long)",
			              Sse41.CompareEqual (LoadI64 (5, -5), LoadI64 (5, 7)), -1, 0);

			CheckArrayI16 ("Sse41.ConvertToVector128Int16(sbyte)",
			              ToArrayI16 (Sse41.ConvertToVector128Int16 (
				              LoadI8 (-1, 2, -3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0))),
			              -1, 2, -3, 4, 5, 6, 7, 8);
			CheckArrayI16 ("Sse41.ConvertToVector128Int16(byte)",
			              ToArrayI16 (Sse41.ConvertToVector128Int16 (
				              LoadU8 (200, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0))),
			              200, 2, 3, 4, 5, 6, 7, 8);
			CheckLanesI32 ("Sse41.ConvertToVector128Int32(short)",
			              Sse41.ConvertToVector128Int32 (LoadI16 (-1, 2, -3, 4, 5, 6, 7, 8)),
			              -1, 2, -3, 4);
			CheckLanesI64 ("Sse41.ConvertToVector128Int64(int)",
			              Sse41.ConvertToVector128Int64 (LoadI32 (-1, 2, 3, 4)), -1, 2);

			CheckLanes ("Sse41.DotProduct(float, all lanes)",
			           Sse41.DotProduct (Load (1f, 2f, 3f, 4f), Load (5f, 6f, 7f, 8f), 0xFF),
			           70f, 70f, 70f, 70f);
			CheckLanes ("Sse41.DotProduct(float, partial)",
			           Sse41.DotProduct (Load (1f, 2f, 3f, 4f), Load (5f, 6f, 7f, 8f), 0x31),
			           17f, 0f, 0f, 0f);
			CheckLanesF64 ("Sse41.DotProduct(double)",
			              Sse41.DotProduct (LoadF64 (2.0, 3.0), LoadF64 (4.0, 5.0), 0x33),
			              23.0, 23.0);

			Vector128<int> extractSrc = LoadI32 (11, 22, 33, 44);

			Check ("Sse41.Extract(int)", Sse41.Extract (extractSrc, 2) == 33);
			Check ("Sse41.Extract(int) wraps the index",
			      Sse41.Extract (extractSrc, 6) == 33);
			Check ("Sse41.Extract(float)", Sse41.Extract (a, 1) == -2f);
			Check ("Sse41.Extract(long)", Sse41.Extract (LoadI64 (100, 200), 1) == 200);

			CheckLanesI32 ("Sse41.Insert(int)", Sse41.Insert (LoadI32 (1, 2, 3, 4), 99, 2),
			              1, 2, 99, 4);
			CheckLanesI32 ("Sse41.Insert(int) wraps the index",
			              Sse41.Insert (LoadI32 (1, 2, 3, 4), 55, 6), 1, 2, 55, 4);
			CheckLanes ("Sse41.Insert(float)",
			           Sse41.Insert (Load (1f, 2f, 3f, 4f), Load (10f, 20f, 30f, 40f), 0x98),
			           1f, 30f, 3f, 0f);

			CheckArrayI8 ("Sse41.Max(sbyte)",
			             ToArrayI8 (Sse41.Max (
				             LoadI8 (-5, 120, -1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
				             LoadI8 (10, -120, -1, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))),
			             10, 120, -1, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
			CheckArrayI8 ("Sse41.Min(sbyte)",
			             ToArrayI8 (Sse41.Min (
				             LoadI8 (-5, 120, -1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
				             LoadI8 (10, -120, -1, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))),
			             -5, -120, -1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

			CheckArrayU16 ("Sse41.Max(ushort)",
			              ToArrayU16 (Sse41.Max (LoadU16 (60000, 100, 0, 0, 0, 0, 0, 0),
			                                     LoadU16 (50000, 200, 0, 0, 0, 0, 0, 0))),
			              60000, 200, 0, 0, 0, 0, 0, 0);
			CheckArrayU16 ("Sse41.Min(ushort)",
			              ToArrayU16 (Sse41.Min (LoadU16 (60000, 100, 0, 0, 0, 0, 0, 0),
			                                     LoadU16 (50000, 200, 0, 0, 0, 0, 0, 0))),
			              50000, 100, 0, 0, 0, 0, 0, 0);

			CheckLanesU32 ("Sse41.Max(uint)",
			              Sse41.Max (LoadU32 (4000000000, 5, 6, 7), LoadU32 (1000000000, 10, 6, 3)),
			              4000000000, 10, 6, 7);
			CheckLanesU32 ("Sse41.Min(uint)",
			              Sse41.Min (LoadU32 (4000000000, 5, 6, 7), LoadU32 (1000000000, 10, 6, 3)),
			              1000000000, 5, 6, 3);

			CheckLanesI32 ("Sse41.Max(int)",
			              Sse41.Max (LoadI32 (-5, 10, 3, -100), LoadI32 (2, -10, 3, 50)),
			              2, 10, 3, 50);
			CheckLanesI32 ("Sse41.Min(int)",
			              Sse41.Min (LoadI32 (-5, 10, 3, -100), LoadI32 (2, -10, 3, 50)),
			              -5, -10, 3, -100);

			CheckArrayU16 ("Sse41.MinHorizontal",
			              ToArrayU16 (Sse41.MinHorizontal (
				              LoadU16 (50, 10, 30, 10, 5, 60, 5, 5))),
			              5, 4, 0, 0, 0, 0, 0, 0);

			{
				Vector128<byte> msadLeft = LoadU8 (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
				                                   15, 16);
				Vector128<byte> msadRight = LoadU8 (0, 0, 0, 0, 50, 50, 50, 50, 0, 0, 0, 0, 0, 0,
				                                    0, 0);

				CheckArrayU16 ("Sse41.MultipleSumAbsoluteDifferences(mask=0)",
				              ToArrayU16 (Sse41.MultipleSumAbsoluteDifferences (
					              msadLeft, msadRight, 0)),
				              10, 14, 18, 22, 26, 30, 34, 38);
				CheckArrayU16 ("Sse41.MultipleSumAbsoluteDifferences(mask=5)",
				              ToArrayU16 (Sse41.MultipleSumAbsoluteDifferences (
					              msadLeft, msadRight, 5)),
				              174, 170, 166, 162, 158, 154, 150, 146);
			}

			CheckLanesI64 ("Sse41.Multiply(int)",
			              Sse41.Multiply (LoadI32 (3, 100, -4, 200), LoadI32 (7, 100, 5, 100)),
			              21, -20);
			CheckLanesI32 ("Sse41.MultiplyLow(int)",
			              Sse41.MultiplyLow (LoadI32 (7, -3, 1000, -1000), LoadI32 (6, 5, 3, 7)),
			              42, -15, 3000, -7000);

			CheckArrayU16 ("Sse41.PackUnsignedSaturate",
			              ToArrayU16 (Sse41.PackUnsignedSaturate (
				              LoadI32 (-1, 100, 70000, 5), LoadI32 (65536, 3, 0, 40000))),
			              0, 100, 65535, 5, 65535, 3, 0, 40000);

			unsafe {
				int* rawNp = stackalloc int[8];
				int* np = (int *) (((long) rawNp + 15) & ~15L);
				np[0] = 9; np[1] = 8; np[2] = 7; np[3] = 6;
				CheckLanesI32 ("Sse41.LoadAlignedVector128NonTemporal",
				              Sse41.LoadAlignedVector128NonTemporal (np), 9, 8, 7, 6);
			}

			Vector128<int> allOnes = LoadI32 (-1, -1, -1, -1);
			Vector128<int> notAllOnes = LoadI32 (-1, -1, -1, 0);
			Vector128<int> va = LoadI32 (0x0F, 0, 0, 0);
			Vector128<int> vb = LoadI32 (0xF0, 0, 0, 0);
			Vector128<int> vc = LoadI32 (0x33, 0, 0, 0);
			Vector128<int> vd = LoadI32 (0x03, 0, 0, 0);

			Check ("Sse41.TestAllOnes(all ones)", Sse41.TestAllOnes (allOnes));
			Check ("Sse41.TestAllOnes(not all ones)", !Sse41.TestAllOnes (notAllOnes));
			Check ("Sse41.TestAllZeros(disjoint)", Sse41.TestAllZeros (va, vb));
			Check ("Sse41.TestZ(disjoint)", Sse41.TestZ (va, vb));
			Check ("Sse41.TestAllZeros(overlapping)", !Sse41.TestAllZeros (va, vc));
			Check ("Sse41.TestC(subset of all ones)", Sse41.TestC (allOnes, va));
			Check ("Sse41.TestC(subset)", Sse41.TestC (va, vd));
			Check ("Sse41.TestC(not a subset)", !Sse41.TestC (va, vb));
			Check ("Sse41.TestMixOnesZeros(mixed)", Sse41.TestMixOnesZeros (va, vc));
			Check ("Sse41.TestNotZAndNotC(mixed)", Sse41.TestNotZAndNotC (va, vc));
			Check ("Sse41.TestMixOnesZeros(disjoint)", !Sse41.TestMixOnesZeros (va, vb));
			Check ("Sse41.TestMixOnesZeros(subset)", !Sse41.TestMixOnesZeros (va, vd));
		}

		Check ("Sse42.IsSupported", Sse42.IsSupported);

		if (Sse42.IsSupported) {
			CheckLanesI64 ("Sse42.CompareGreaterThan(long)",
			              Sse42.CompareGreaterThan (LoadI64 (5, -5), LoadI64 (3, -3)), -1, 0);

			// Feeding the same bytes through narrower CRC32C forms must produce the
			// same result on this little-endian target.
			uint crcInit = 0xFFFFFFFFu;
			byte[] check = System.Text.Encoding.ASCII.GetBytes ("123456789");
			uint crc8 = crcInit;

			foreach (byte checkByte in check)
				crc8 = Sse42.Crc32 (crc8, checkByte);
			Check ("Sse42.Crc32(uint, byte) check value", (crc8 ^ 0xFFFFFFFFu) == 0xE3069283u);

			ushort word = 0x1234;
			uint viaBytes = Sse42.Crc32 (Sse42.Crc32 (crcInit, (byte) (word & 0xFF)),
			                             (byte) (word >> 8));
			Check ("Sse42.Crc32(uint, ushort) matches byte-at-a-time",
			      Sse42.Crc32 (crcInit, word) == viaBytes);

			uint dword = 0x12345678u;
			uint viaWords = Sse42.Crc32 (Sse42.Crc32 (crcInit, (ushort) (dword & 0xFFFF)),
			                             (ushort) (dword >> 16));
			Check ("Sse42.Crc32(uint, uint) matches word-at-a-time",
			      Sse42.Crc32 (crcInit, dword) == viaWords);

			ulong qword = 0x0123456789ABCDEFUL;
			uint viaDwords = Sse42.Crc32 (Sse42.Crc32 (crcInit, (uint) (qword & 0xFFFFFFFFu)),
			                              (uint) (qword >> 32));
			Check ("Sse42.Crc32(ulong, ulong) matches dword-at-a-time",
			      Sse42.Crc32 ((ulong) crcInit, qword) == (ulong) viaDwords);

			// EqualAny reports matching positions in the right operand. Verify that
			// indices, masks, and flags all use that orientation.
			Vector128<sbyte> strLeft = LoadI8 (97, 98, 99, 97, 98, 99, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
			Vector128<sbyte> strRight = LoadI8 (120, 99, 98, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

			Check ("Sse42.CompareImplicitLength(CFlag)",
			      Sse42.CompareImplicitLength (strLeft, strRight, ResultsFlag.CFlag,
			                                   StringComparisonMode.EqualAny));
			Check ("Sse42.CompareImplicitLength(OFlag)",
			      !Sse42.CompareImplicitLength (strLeft, strRight, ResultsFlag.OFlag,
			                                    StringComparisonMode.EqualAny));

			Check ("Sse42.CompareImplicitLengthIndex(least significant)",
			      Sse42.CompareImplicitLengthIndex (strLeft, strRight,
			                                        StringComparisonMode.EqualAny) == 1);
			Check ("Sse42.CompareImplicitLengthIndex(most significant)",
			      Sse42.CompareImplicitLengthIndex (
				      strLeft, strRight,
				      StringComparisonMode.EqualAny | StringComparisonMode.MostSignificant) == 2);

			CheckArrayU16 ("Sse42.CompareImplicitLengthBitMask",
			              ToArrayU16 (Sse42.CompareImplicitLengthBitMask (
				              strLeft, strRight, StringComparisonMode.EqualAny)),
			              0x6, 0, 0, 0, 0, 0, 0, 0);
			CheckArrayU8 ("Sse42.CompareImplicitLengthUnitMask",
			             ToArrayU8 (Sse42.CompareImplicitLengthUnitMask (
				             strLeft, strRight, StringComparisonMode.EqualAny)),
			             0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

			Vector128<byte> estrLeft = LoadU8 (10, 20, 30, 40, 99, 99, 99, 99, 99, 99, 99, 99, 99,
			                                   99, 99, 99);
			Vector128<byte> estrRight = LoadU8 (99, 20, 10, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
			                                    99, 99, 99);

			Check ("Sse42.CompareExplicitLength(CFlag)",
			      Sse42.CompareExplicitLength (estrLeft, 4, estrRight, 3, ResultsFlag.CFlag,
			                                   StringComparisonMode.EqualAny));
			Check ("Sse42.CompareExplicitLength(OFlag)",
			      !Sse42.CompareExplicitLength (estrLeft, 4, estrRight, 3, ResultsFlag.OFlag,
			                                    StringComparisonMode.EqualAny));

			Check ("Sse42.CompareExplicitLengthIndex",
			      Sse42.CompareExplicitLengthIndex (estrLeft, 4, estrRight, 3,
			                                        StringComparisonMode.EqualAny) == 1);

			CheckArrayU16 ("Sse42.CompareExplicitLengthBitMask",
			              ToArrayU16 (Sse42.CompareExplicitLengthBitMask (
				              estrLeft, 4, estrRight, 3, StringComparisonMode.EqualAny)),
			              0x6, 0, 0, 0, 0, 0, 0, 0);
			CheckArrayU8 ("Sse42.CompareExplicitLengthUnitMask",
			             ToArrayU8 (Sse42.CompareExplicitLengthUnitMask (
				             estrLeft, 4, estrRight, 3, StringComparisonMode.EqualAny)),
			             0, 255, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);

			Vector128<short> wordLeft = LoadI16 (100, 200, 300, 400, 0, 0, 0, 0);
			Vector128<short> wordRight = LoadI16 (300, 0, 0, 0, 0, 0, 0, 0);

			Check ("Sse42.CompareImplicitLengthIndex(short)",
			      Sse42.CompareImplicitLengthIndex (wordLeft, wordRight,
			                                        StringComparisonMode.EqualAny) == 0);
		}

		if (Avx.IsSupported) {
			Check ("Avx.IsSupported", Avx.IsSupported);

			Vector256<float> fa = LoadF256 (1f, 2f, 3f, 4f, 5f, 6f, 7f, 8f);
			Vector256<float> fb = LoadF256 (10f, 20f, 30f, 40f, 50f, 60f, 70f, 80f);

			CheckLanesF256 ("Avx.Add", Avx.Add (fa, fb), 11f, 22f, 33f, 44f, 55f, 66f, 77f, 88f);
			CheckLanesF256 ("Avx.Subtract", Avx.Subtract (fa, fb), -9f, -18f, -27f, -36f, -45f,
			               -54f, -63f, -72f);
			CheckLanesF256 ("Avx.Multiply", Avx.Multiply (fa, fb), 10f, 40f, 90f, 160f, 250f, 360f,
			               490f, 640f);
			CheckLanesF256 ("Avx.Divide", Avx.Divide (fb, fa), 10f, 10f, 10f, 10f, 10f, 10f, 10f,
			               10f);

			Vector256<float> fmax = LoadF256 (1f, -2f, 3f, 4f, 5f, -6f, 7f, -8f);
			Vector256<float> fzero = LoadF256 (0f, 0f, 0f, 0f, 0f, 0f, 0f, 0f);
			CheckLanesF256 ("Avx.Max", Avx.Max (fmax, fzero), 1f, 0f, 3f, 4f, 5f, 0f, 7f, 0f);
			CheckLanesF256 ("Avx.Min", Avx.Min (fmax, fzero), 0f, -2f, 0f, 0f, 0f, -6f, 0f, -8f);

			CheckLanesF256 ("Avx.Sqrt", Avx.Sqrt (LoadF256 (4f, 9f, 16f, 25f, 36f, 49f, 64f, 81f)),
			               2f, 3f, 4f, 5f, 6f, 7f, 8f, 9f);

			float avxRecip =
				ToArrayF256 (Avx.Reciprocal (LoadF256 (4f, 4f, 4f, 4f, 4f, 4f, 4f, 4f))) [0];
			Check ("Avx.Reciprocal is approximately 1/4", Math.Abs (avxRecip - 0.25f) < 0.001f);
			float rsqrt =
				ToArrayF256 (Avx.ReciprocalSqrt (LoadF256 (4f, 4f, 4f, 4f, 4f, 4f, 4f, 4f))) [0];
			Check ("Avx.ReciprocalSqrt is approximately 1/2", Math.Abs (rsqrt - 0.5f) < 0.001f);

			Vector256<float> bitsA = LoadF256 (0xFFFFFFFFu.ToSingle (), 0, 0, 0, 0, 0, 0, 0);
			Vector256<float> bitsB = LoadF256 (0x0000FFFFu.ToSingle (), 0, 0, 0, 0, 0, 0, 0);
			Check ("Avx.And",
			      unchecked ((uint) BitConverter.SingleToInt32Bits (
					      ToArrayF256 (Avx.And (bitsA, bitsB)) [0])) == 0x0000FFFFu);
			Check ("Avx.Or",
			      unchecked ((uint) BitConverter.SingleToInt32Bits (
					      ToArrayF256 (Avx.Or (bitsA, bitsB)) [0])) == 0xFFFFFFFFu);
			Check ("Avx.Xor",
			      unchecked ((uint) BitConverter.SingleToInt32Bits (
					      ToArrayF256 (Avx.Xor (bitsA, bitsB)) [0])) == 0xFFFF0000u);
			Check ("Avx.AndNot",
			      unchecked ((uint) BitConverter.SingleToInt32Bits (
					      ToArrayF256 (Avx.AndNot (bitsB, bitsA)) [0])) == 0xFFFF0000u);

			CheckLanesF256 ("Avx.AddSubtract", Avx.AddSubtract (fa, fb), -9f, 22f, -27f, 44f, -45f,
			               66f, -63f, 88f);
			CheckLanesF256 ("Avx.HorizontalAdd", Avx.HorizontalAdd (fa, fb), 3f, 7f, 30f, 70f, 11f,
			               15f, 110f, 150f);
			CheckLanesF256 ("Avx.HorizontalSubtract", Avx.HorizontalSubtract (fa, fb), -1f, -1f,
			               -10f, -10f, -1f, -1f, -10f, -10f);

			bool cmpAllTrue = true, cmpAllFalse = true;
			float[] lt =
				ToArrayF256 (Avx.Compare (fa, fb, FloatComparisonMode.LessThanOrderedSignaling));
			float[] ge =
				ToArrayF256 (Avx.Compare (fb, fa, FloatComparisonMode.LessThanOrderedSignaling));
			for (int i = 0; i < 8; i++) {
				cmpAllTrue &= BitConverter.SingleToInt32Bits (lt [i]) == -1;
				cmpAllFalse &= BitConverter.SingleToInt32Bits (ge [i]) == 0;
			}
			Check ("Avx.Compare(Vector256<float>, LessThan) is true everywhere", cmpAllTrue);
			Check ("Avx.Compare(Vector256<float>, LessThan) is false everywhere", cmpAllFalse);

			Vector128<float> lo128 = Load (1f, 2f, 3f, 4f);
			Vector128<float> hi128 = Load (10f, 20f, 30f, 40f);
			float[] lt128 = ToArray (
				Avx.Compare (lo128, hi128, FloatComparisonMode.LessThanOrderedSignaling));
			Check ("Avx.Compare(Vector128<float>, LessThan)",
			      BitConverter.SingleToInt32Bits (lt128 [0]) == -1);

			float[] cmpScalar =
				ToArray (Avx.CompareScalar (lo128, hi128, FloatComparisonMode.LessThanOrderedSignaling));
			Check ("Avx.CompareScalar lane 0 compares",
			      BitConverter.SingleToInt32Bits (cmpScalar [0]) == -1);
			Check ("Avx.CompareScalar leaves the other lanes",
			      cmpScalar [1] == 2f && cmpScalar [2] == 3f && cmpScalar [3] == 4f);

			Vector256<float> round = LoadF256 (1.2f, -1.2f, 2.7f, -2.7f, 0.4f, -0.4f, 3.6f, -3.6f);
			CheckLanesF256 ("Avx.Ceiling", Avx.Ceiling (round), 2f, -1f, 3f, -2f, 1f, 0f, 4f, -3f);
			CheckLanesF256 ("Avx.Floor", Avx.Floor (round), 1f, -2f, 2f, -3f, 0f, -1f, 3f, -4f);
			CheckLanesF256 ("Avx.RoundToNearestInteger", Avx.RoundToNearestInteger (round), 1f, -1f,
			               3f, -3f, 0f, 0f, 4f, -4f);
			CheckLanesF256 ("Avx.RoundToZero", Avx.RoundToZero (round), 1f, -1f, 2f, -2f, 0f, 0f, 3f,
			               -3f);
			Check ("Avx.RoundToNegativeInfinity lane 0",
			      ToArrayF256 (Avx.RoundToNegativeInfinity (round)) [0] == 1f);
			Check ("Avx.RoundToPositiveInfinity lane 0",
			      ToArrayF256 (Avx.RoundToPositiveInfinity (round)) [0] == 2f);
			Check ("Avx.RoundCurrentDirection lane 0",
			      ToArrayF256 (Avx.RoundCurrentDirection (round)) [0] == 1f);

			CheckLanesF256 ("Avx.Blend", Avx.Blend (fa, fb, 0xAA), 1f, 20f, 3f, 40f, 5f, 60f, 7f,
			               80f);

			Vector256<float> blendMask =
				LoadF256 (-1f, 1f, -1f, 1f, -1f, 1f, -1f, 1f);
			CheckLanesF256 ("Avx.BlendVariable", Avx.BlendVariable (fa, fb, blendMask), 10f, 2f, 30f,
			               4f, 50f, 6f, 70f, 8f);

			float broadcastSource = 42f;
			CheckLanes ("Avx.BroadcastScalarToVector128", Avx.BroadcastScalarToVector128 (&broadcastSource),
			           42f, 42f, 42f, 42f);
			CheckLanesF256 ("Avx.BroadcastScalarToVector256",
			               Avx.BroadcastScalarToVector256 (&broadcastSource), 42f, 42f, 42f, 42f, 42f,
			               42f, 42f, 42f);

			double broadcastD = 3.5;
			CheckLanesD256 ("Avx.BroadcastScalarToVector256(double)",
			                Avx.BroadcastScalarToVector256 (&broadcastD), 3.5, 3.5, 3.5, 3.5);

			Vector128<float> broadcastHalf = Load (1f, 2f, 3f, 4f);
			float* broadcastHalfBuffer = stackalloc float[4];
			Sse.Store (broadcastHalfBuffer, broadcastHalf);
			CheckLanesF256 ("Avx.BroadcastVector128ToVector256",
			               Avx.BroadcastVector128ToVector256 (broadcastHalfBuffer), 1f, 2f, 3f, 4f,
			               1f, 2f, 3f, 4f);

			Check ("Avx.ConvertToSingle", Avx.ConvertToSingle (LoadF256 (5f, 0, 0, 0, 0, 0, 0, 0)) == 5f);

			CheckLanesI32 ("Avx.ConvertToVector128Int32",
			              Avx.ConvertToVector128Int32 (LoadD256 (1.7, -1.7, 2.5, -2.5)), 2, -2, 2,
			              -2);
			CheckLanes ("Avx.ConvertToVector128Single",
			           Avx.ConvertToVector128Single (LoadD256 (1.5, 2.5, 3.5, 4.5)), 1.5f, 2.5f, 3.5f,
			           4.5f);
			CheckLanesI256 ("Avx.ConvertToVector256Int32",
			                Avx.ConvertToVector256Int32 (LoadF256 (1.7f, -1.7f, 2.5f, -2.5f, 3.5f,
			                                                       -3.5f, 0.4f, -0.4f)),
			                2, -2, 2, -2, 4, -4, 0, 0);
			CheckLanesF256 ("Avx.ConvertToVector256Single",
			                Avx.ConvertToVector256Single (
					                LoadI256 (1, -1, 2, -2, 3, -3, 1000000, -1000000)),
			                1f, -1f, 2f, -2f, 3f, -3f, 1000000f, -1000000f);
			CheckLanesD256 ("Avx.ConvertToVector256Double(float)",
			                Avx.ConvertToVector256Double (Load (1f, 2f, 3f, 4f)), 1, 2, 3, 4);
			CheckLanesD256 ("Avx.ConvertToVector256Double(int)",
			                Avx.ConvertToVector256Double (LoadI32 (5, -5, 100, -100)), 5, -5, 100,
			                -100);
			CheckLanesI32 ("Avx.ConvertToVector128Int32WithTruncation",
			              Avx.ConvertToVector128Int32WithTruncation (LoadD256 (1.9, -1.9, 2.1, -2.1)),
			              1, -1, 2, -2);
			CheckLanesI256 ("Avx.ConvertToVector256Int32WithTruncation",
			                Avx.ConvertToVector256Int32WithTruncation (
					                LoadF256 (1.9f, -1.9f, 2.1f, -2.1f, 3.9f, -3.9f, 0.9f, -0.9f)),
			                1, -1, 2, -2, 3, -3, 0, 0);

			Vector256<float> dpLeft = LoadF256 (1f, 2f, 3f, 4f, 100f, 200f, 300f, 400f);
			Vector256<float> dpRight = LoadF256 (1f, 1f, 1f, 1f, 1f, 1f, 1f, 1f);
			CheckLanesF256 ("Avx.DotProduct", Avx.DotProduct (dpLeft, dpRight, 0xF1), 10f, 0f, 0f,
			               0f, 1000f, 0f, 0f, 0f);

			CheckLanesF256 ("Avx.DuplicateEvenIndexed", Avx.DuplicateEvenIndexed (fa), 1f, 1f, 3f,
			               3f, 5f, 5f, 7f, 7f);
			CheckLanesF256 ("Avx.DuplicateOddIndexed", Avx.DuplicateOddIndexed (fa), 2f, 2f, 4f, 4f,
			               6f, 6f, 8f, 8f);
			CheckLanesD256 ("Avx.DuplicateEvenIndexed(double)",
			                Avx.DuplicateEvenIndexed (LoadD256 (1, 2, 3, 4)), 1, 1, 3, 3);

			CheckLanes ("Avx.ExtractVector128 low", Avx.ExtractVector128 (fa, 0), 1f, 2f, 3f, 4f);
			CheckLanes ("Avx.ExtractVector128 high", Avx.ExtractVector128 (fa, 1), 5f, 6f, 7f, 8f);
			CheckLanes ("Avx.ExtractVector128 wraps", Avx.ExtractVector128 (fa, 3), 5f, 6f, 7f, 8f);

			float* extractBuffer = stackalloc float[4];
			Avx.ExtractVector128 (extractBuffer, fa, 1);
			CheckLanes ("Avx.ExtractVector128(store)", Sse.LoadVector128 (extractBuffer), 5f, 6f,
			           7f, 8f);

			Vector128<float> insertData = Load (90f, 91f, 92f, 93f);
			CheckLanesF256 ("Avx.InsertVector128 low", Avx.InsertVector128 (fa, insertData, 0), 90f,
			               91f, 92f, 93f, 5f, 6f, 7f, 8f);
			CheckLanesF256 ("Avx.InsertVector128 high", Avx.InsertVector128 (fa, insertData, 1), 1f,
			               2f, 3f, 4f, 90f, 91f, 92f, 93f);

			float* insertBuffer = stackalloc float[4] { 190f, 191f, 192f, 193f };
			CheckLanesF256 ("Avx.InsertVector128(load)", Avx.InsertVector128 (fa, insertBuffer, 0),
			               190f, 191f, 192f, 193f, 5f, 6f, 7f, 8f);

			CheckLanes ("Avx.GetLowerHalf", Avx.GetLowerHalf (fa), 1f, 2f, 3f, 4f);
			CheckLanes ("Avx.ExtendToVector256 keeps its low half",
			           Avx.GetLowerHalf (Avx.ExtendToVector256 (insertData)), 90f, 91f, 92f, 93f);

			float* raw = stackalloc float[16];
			float* aligned = (float *) (((long) raw + 31) & ~31L);
			Avx.StoreAligned (aligned, fa);
			CheckLanesF256 ("Avx.LoadAlignedVector256/StoreAligned", Avx.LoadAlignedVector256 (aligned),
			               1f, 2f, 3f, 4f, 5f, 6f, 7f, 8f);
			Avx.StoreAlignedNonTemporal (aligned, fb);
			CheckLanesF256 ("Avx.StoreAlignedNonTemporal", Avx.LoadAlignedVector256 (aligned), 10f,
			               20f, 30f, 40f, 50f, 60f, 70f, 80f);

			int* dquBuffer = stackalloc int[8];
			Avx.Store (dquBuffer, LoadI256 (1, 2, 3, 4, 5, 6, 7, 8));
			CheckLanesI256 ("Avx.LoadDquVector256", Avx.LoadDquVector256 (dquBuffer), 1, 2, 3, 4, 5,
			               6, 7, 8);

			Vector256<float> maskLoadMask = LoadF256 (-1f, 1f, -1f, 1f, -1f, 1f, -1f, 1f);
			float* maskLoadBuffer = stackalloc float[8] { 1f, 2f, 3f, 4f, 5f, 6f, 7f, 8f };
			CheckLanesF256 ("Avx.MaskLoad", Avx.MaskLoad (maskLoadBuffer, maskLoadMask), 1f, 0f, 3f,
			               0f, 5f, 0f, 7f, 0f);

			float* maskStoreBuffer = stackalloc float[8] { 999f, 999f, 999f, 999f, 999f, 999f, 999f,
				999f };
			Avx.MaskStore (maskStoreBuffer, maskLoadMask, fb);
			Check ("Avx.MaskStore",
			      maskStoreBuffer [0] == 10f && maskStoreBuffer [1] == 999f
			      && maskStoreBuffer [2] == 30f && maskStoreBuffer [3] == 999f
			      && maskStoreBuffer [4] == 50f && maskStoreBuffer [5] == 999f
			      && maskStoreBuffer [6] == 70f && maskStoreBuffer [7] == 999f);

			Vector256<float> movmskValue =
				LoadF256 (-1f, 2f, -3f, 4f, -5f, 6f, -7f, 8f);
			Check ("Avx.MoveMask", Avx.MoveMask (movmskValue) == 0x55);
			Check ("Avx.MoveMask(double)",
			      Avx.MoveMask (LoadD256 (-1, 2, -3, 4)) == 0x5);

			CheckLanesF256 ("Avx.Permute(Vector256<float>)",
			                Avx.Permute (LoadF256 (10f, 20f, 30f, 40f, 50f, 60f, 70f, 80f), 0xE1),
			                20f, 10f, 30f, 40f, 60f, 50f, 70f, 80f);
			CheckLanes ("Avx.Permute(Vector128<float>)",
			           Avx.Permute (Load (10f, 20f, 30f, 40f), 0xE1), 20f, 10f, 30f, 40f);
			CheckLanesD256 ("Avx.Permute(Vector256<double>)",
			                Avx.Permute (LoadD256 (10, 20, 30, 40), 0b1101), 20, 10, 40, 40);

			Vector256<int> permuteVarControl = LoadI256 (1, 0, 2, 3, 1, 0, 2, 3);
			CheckLanesF256 ("Avx.PermuteVar",
			                Avx.PermuteVar (LoadF256 (10f, 20f, 30f, 40f, 50f, 60f, 70f, 80f),
			                                permuteVarControl),
			                20f, 10f, 30f, 40f, 60f, 50f, 70f, 80f);

			Vector256<float> p2left = LoadF256 (1f, 2f, 3f, 4f, 5f, 6f, 7f, 8f);
			Vector256<float> p2right = LoadF256 (21f, 22f, 23f, 24f, 25f, 26f, 27f, 28f);
			CheckLanesF256 ("Avx.Permute2x128 low+low", Avx.Permute2x128 (p2left, p2right, 0x20),
			               1f, 2f, 3f, 4f, 21f, 22f, 23f, 24f);
			CheckLanesF256 ("Avx.Permute2x128 high+high", Avx.Permute2x128 (p2left, p2right, 0x31),
			               5f, 6f, 7f, 8f, 25f, 26f, 27f, 28f);
			CheckLanesF256 ("Avx.Permute2x128 zeroed high",
			               Avx.Permute2x128 (p2left, p2right, 0x81), 5f, 6f, 7f, 8f, 0f, 0f, 0f, 0f);
			CheckLanesF256 ("Avx.Permute2x128 zeroed low",
			               Avx.Permute2x128 (p2left, p2right, 0x08), 0f, 0f, 0f, 0f, 1f, 2f, 3f, 4f);

			Vector256<float> shufLeft = LoadF256 (10f, 20f, 30f, 40f, 50f, 60f, 70f, 80f);
			Vector256<float> shufRight = LoadF256 (110f, 120f, 130f, 140f, 150f, 160f, 170f, 180f);
			CheckLanesF256 ("Avx.Shuffle(float)", Avx.Shuffle (shufLeft, shufRight, 0x1B), 40f, 30f,
			               120f, 110f, 80f, 70f, 160f, 150f);
			Vector256<double> shufDLeft = LoadD256 (10, 20, 30, 40);
			Vector256<double> shufDRight = LoadD256 (110, 120, 130, 140);
			CheckLanesD256 ("Avx.Shuffle(double)", Avx.Shuffle (shufDLeft, shufDRight, 0b1010), 10,
			               120, 30, 140);

			CheckLanesI256 ("Avx.SetVector256(int)",
			                Avx.SetVector256 (7, 6, 5, 4, 3, 2, 1, 0), 0, 1, 2, 3, 4, 5, 6, 7);
			CheckLanesF256 ("Avx.SetVector256(float)",
			                Avx.SetVector256 (7f, 6f, 5f, 4f, 3f, 2f, 1f, 0f), 0f, 1f, 2f, 3f, 4f,
			                5f, 6f, 7f);
			CheckLanesD256 ("Avx.SetVector256(double)", Avx.SetVector256 (3.0, 2.0, 1.0, 0.0), 0, 1,
			               2, 3);

			CheckLanesF256 ("Avx.SetAllVector256", Avx.SetAllVector256 (9f), 9f, 9f, 9f, 9f, 9f, 9f,
			               9f, 9f);
			CheckLanesF256 ("Avx.SetHighLow", Avx.SetHighLow (insertData, lo128), 1f, 2f, 3f, 4f,
			               90f, 91f, 92f, 93f);
			CheckLanesF256 ("Avx.SetZeroVector256", Avx.SetZeroVector256<float> (), 0f, 0f, 0f, 0f,
			               0f, 0f, 0f, 0f);

			CheckLanesF256 ("Avx.StaticCast", Avx.StaticCast<int, float> (LoadI256 (
					                                BitConverter.SingleToInt32Bits (1f),
					                                BitConverter.SingleToInt32Bits (2f), 0, 0, 0, 0,
					                                0, 0)),
			                1f, 2f, 0f, 0f, 0f, 0f, 0f, 0f);

			CheckLanesF256 ("Avx.UnpackLow", Avx.UnpackLow (fa, fb), 1f, 10f, 2f, 20f, 5f, 50f, 6f,
			               60f);
			CheckLanesF256 ("Avx.UnpackHigh", Avx.UnpackHigh (fa, fb), 3f, 30f, 4f, 40f, 7f, 70f, 8f,
			               80f);

			Vector128<float> testAllZero = Load (0f, 0f, 0f, 0f);
			Vector128<float> testAllOnes = Load (
				BitConverter.Int32BitsToSingle (-1), BitConverter.Int32BitsToSingle (-1),
				BitConverter.Int32BitsToSingle (-1), BitConverter.Int32BitsToSingle (-1));
			Check ("Avx.TestZ(Vector128<float>) all zero", Avx.TestZ (testAllZero, testAllOnes));
			Check ("Avx.TestC(Vector128<float>) all ones mask", Avx.TestC (testAllOnes, testAllZero));
			Check ("Avx.TestNotZAndNotC(Vector128<float>) mixed is false on all-zero value",
			      !Avx.TestNotZAndNotC (testAllZero, testAllOnes));

			Vector256<int> testAllZero256 = LoadI256 (0, 0, 0, 0, 0, 0, 0, 0);
			Vector256<int> testAllOnes256 = LoadI256 (-1, -1, -1, -1, -1, -1, -1, -1);
			Check ("Avx.TestZ<int>(Vector256) all zero", Avx.TestZ (testAllZero256, testAllOnes256));
			Check ("Avx.TestC<int>(Vector256) all ones mask",
			      Avx.TestC (testAllOnes256, testAllZero256));
		}

		if (Avx2.IsSupported) {
			Check ("Avx2.IsSupported", Avx2.IsSupported);

			sbyte[] seq8 = new sbyte[32];
			sbyte[] negSeq8 = new sbyte[32];
			for (int i = 0; i < 32; i++) {
				seq8 [i] = (sbyte) (i + 1);
				negSeq8 [i] = (sbyte) -(i + 1);
			}
			Vector256<sbyte> i8seq = LoadI8x256 (seq8);
			Vector256<sbyte> i8negSeq = LoadI8x256 (negSeq8);

			byte[] absResult = ToArrayU8x256 (Avx2.Abs (i8negSeq));
			bool absOk = true;
			for (int i = 0; i < 32; i++)
				absOk &= absResult [i] == (byte) (i + 1);
			Check ("Avx2.Abs", absOk);

			sbyte[] addResult = ToArrayI8x256 (Avx2.Add (i8seq, i8seq));
			bool addOk = true;
			for (int i = 0; i < 32; i++)
				addOk &= addResult [i] == (sbyte) (2 * (i + 1));
			Check ("Avx2.Add", addOk);

			sbyte[] hundred = new sbyte[32];
			for (int i = 0; i < 32; i++) hundred [i] = 100;
			Vector256<sbyte> i8hundred = LoadI8x256 (hundred);
			sbyte[] addSatResult = ToArrayI8x256 (Avx2.AddSaturate (i8hundred, i8hundred));
			bool addSatOk = true;
			for (int i = 0; i < 32; i++)
				addSatOk &= addSatResult [i] == 127;
			Check ("Avx2.AddSaturate", addSatOk);

			byte[] twoHundred = new byte[32];
			for (int i = 0; i < 32; i++) twoHundred [i] = 200;
			Vector256<byte> u8twoHundred = LoadU8x256 (twoHundred);
			byte[] addSatUResult = ToArrayU8x256 (Avx2.AddSaturate (u8twoHundred, u8twoHundred));
			bool addSatUOk = true;
			for (int i = 0; i < 32; i++)
				addSatUOk &= addSatUResult [i] == 255;
			Check ("Avx2.AddSaturate(byte)", addSatUOk);

			sbyte[] subResult = ToArrayI8x256 (Avx2.Subtract (i8seq, i8seq));
			bool subOk = true;
			for (int i = 0; i < 32; i++)
				subOk &= subResult [i] == 0;
			Check ("Avx2.Subtract", subOk);

			sbyte[] negHundred = new sbyte[32];
			for (int i = 0; i < 32; i++) negHundred [i] = -100;
			sbyte[] subSatResult =
				ToArrayI8x256 (Avx2.SubtractSaturate (LoadI8x256 (negHundred), i8hundred));
			bool subSatOk = true;
			for (int i = 0; i < 32; i++)
				subSatOk &= subSatResult [i] == -128;
			Check ("Avx2.SubtractSaturate", subSatOk);

			CheckArrayI8x256 ("Avx2.AlignRight(mask=0) is right",
			                 ToArrayI8x256 (Avx2.AlignRight (i8seq, i8negSeq, 0)), negSeq8);
			CheckArrayI8x256 ("Avx2.AlignRight(mask=16) is left",
			                 ToArrayI8x256 (Avx2.AlignRight (i8seq, i8negSeq, 16)), seq8);
			CheckArrayI8x256 ("Avx2.AlignRight(mask=32) is zero",
			                 ToArrayI8x256 (Avx2.AlignRight (i8seq, i8negSeq, 32)), new sbyte[32]);

			Vector256<int> bitsA = LoadI256 (0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
			Vector256<int> bitsB = LoadI256 (0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F);
			CheckLanesI256 ("Avx2.And", Avx2.And (bitsA, bitsB), 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,
			               0x0F, 0x0F);
			CheckLanesI256 ("Avx2.AndNot", Avx2.AndNot (bitsB, bitsA), 0xF0, 0xF0, 0xF0, 0xF0, 0xF0,
			               0xF0, 0xF0, 0xF0);
			CheckLanesI256 ("Avx2.Or", Avx2.Or (bitsA, bitsB), 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
			               0xFF, 0xFF);
			CheckLanesI256 ("Avx2.Xor", Avx2.Xor (bitsA, bitsB), 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0,
			               0xF0, 0xF0);

			byte[] avgAArr = new byte[32];
			byte[] avgBArr = new byte[32];
			for (int i = 0; i < 32; i++) { avgAArr [i] = 10; avgBArr [i] = 21; }
			byte[] avgResult =
				ToArrayU8x256 (Avx2.Average (LoadU8x256 (avgAArr), LoadU8x256 (avgBArr)));
			bool avgOk = true;
			for (int i = 0; i < 32; i++)
				avgOk &= avgResult [i] == 16;
			Check ("Avx2.Average", avgOk);

			ushort[] avgU16A = new ushort[16];
			ushort[] avgU16B = new ushort[16];
			for (int i = 0; i < 16; i++) { avgU16A [i] = 1000; avgU16B [i] = 2001; }
			ushort[] avgU16Result =
				ToArrayU16x256 (Avx2.Average (LoadU16x256 (avgU16A), LoadU16x256 (avgU16B)));
			bool avgU16Ok = true;
			for (int i = 0; i < 16; i++)
				avgU16Ok &= avgU16Result [i] == 1501;
			Check ("Avx2.Average(ushort)", avgU16Ok);

			Vector256<int> blendLeftI = LoadI256 (1, 2, 3, 4, 5, 6, 7, 8);
			Vector256<int> blendRightI = LoadI256 (10, 20, 30, 40, 50, 60, 70, 80);
			CheckLanesI256 ("Avx2.Blend(int)", Avx2.Blend (blendLeftI, blendRightI, 0xAA), 1, 20, 3,
			               40, 5, 60, 7, 80);

			short[] blendLeftArr = new short[16];
			short[] blendRightArr = new short[16];
			for (int i = 0; i < 16; i++) {
				blendLeftArr [i] = (short) (i + 1);
				blendRightArr [i] = (short) (100 + i);
			}
			short[] blendResult = ToArrayI16x256 (
				Avx2.Blend (LoadI16x256 (blendLeftArr), LoadI16x256 (blendRightArr), 0xAA));
			bool blendOk = true;
			for (int i = 0; i < 16; i++)
				blendOk &= blendResult [i] == (i % 2 == 1 ? blendRightArr [i] : blendLeftArr [i]);
			Check ("Avx2.Blend(short)", blendOk);

			sbyte[] bvLeftArr = new sbyte[32];
			sbyte[] bvRightArr = new sbyte[32];
			sbyte[] bvMaskArr = new sbyte[32];
			for (int i = 0; i < 32; i++) {
				bvLeftArr [i] = 1;
				bvRightArr [i] = 2;
				bvMaskArr [i] = (sbyte) (i % 2 == 0 ? 0 : -1);
			}
			sbyte[] bvResult = ToArrayI8x256 (Avx2.BlendVariable (
				LoadI8x256 (bvLeftArr), LoadI8x256 (bvRightArr), LoadI8x256 (bvMaskArr)));
			bool bvOk = true;
			for (int i = 0; i < 32; i++)
				bvOk &= bvResult [i] == (i % 2 == 0 ? (sbyte) 1 : (sbyte) 2);
			Check ("Avx2.BlendVariable", bvOk);

			Vector128<int> bcastSrc128 = LoadI32 (42, 0, 0, 0);
			CheckLanesI32 ("Avx2.BroadcastScalarToVector128",
			              Avx2.BroadcastScalarToVector128 (bcastSrc128), 42, 42, 42, 42);
			CheckLanesI256 ("Avx2.BroadcastScalarToVector256",
			               Avx2.BroadcastScalarToVector256 (bcastSrc128), 42, 42, 42, 42, 42, 42, 42,
			               42);

			int[] bcastHalf = { 1, 2, 3, 4 };
			fixed (int* bcastHalfPtr = bcastHalf)
				CheckLanesI256 ("Avx2.BroadcastVector128ToVector256",
				               Avx2.BroadcastVector128ToVector256 (bcastHalfPtr), 1, 2, 3, 4, 1, 2,
				               3, 4);

			Vector256<int> cmpA = LoadI256 (1, 2, 3, 4, 5, 6, 7, 8);
			Vector256<int> cmpB = LoadI256 (1, 0, 3, 0, 5, 0, 7, 0);
			CheckLanesI256 ("Avx2.CompareEqual", Avx2.CompareEqual (cmpA, cmpB), -1, 0, -1, 0, -1, 0,
			               -1, 0);
			CheckLanesI256 ("Avx2.CompareGreaterThan", Avx2.CompareGreaterThan (cmpA, cmpB), 0, -1, 0,
			               -1, 0, -1, 0, -1);

			Check ("Avx2.ConvertToDouble", Avx2.ConvertToDouble (LoadD256 (7.5, 0, 0, 0)) == 7.5);
			Check ("Avx2.ConvertToInt32",
			      Avx2.ConvertToInt32 (LoadI256 (99, 0, 0, 0, 0, 0, 0, 0)) == 99);
			Check ("Avx2.ConvertToUInt32",
			      Avx2.ConvertToUInt32 (LoadU256 (99u, 0, 0, 0, 0, 0, 0, 0)) == 99u);

			sbyte[] convSrc8 = new sbyte[16];
			for (int i = 0; i < 16; i++) convSrc8 [i] = (sbyte) (i - 8);
			short[] conv16Result = ToArrayI16x256 (Avx2.ConvertToVector256Int16 (LoadI8 (convSrc8)));
			bool conv16Ok = true;
			for (int i = 0; i < 16; i++)
				conv16Ok &= conv16Result [i] == (short) (i - 8);
			Check ("Avx2.ConvertToVector256Int16", conv16Ok);

			byte[] convSrcU8 = new byte[16];
			for (int i = 0; i < 16; i++) convSrcU8 [i] = (byte) (i + 1);
			ushort[] convU16Result =
				ToArrayU16x256 (Avx2.ConvertToVector256UInt16 (LoadU8 (convSrcU8)));
			bool convU16Ok = true;
			for (int i = 0; i < 16; i++)
				convU16Ok &= convU16Result [i] == (ushort) (i + 1);
			Check ("Avx2.ConvertToVector256UInt16", convU16Ok);

			sbyte[] i32SrcSbyte = new sbyte[16];
			for (int i = 0; i < 16; i++) i32SrcSbyte [i] = (sbyte) (i - 8);
			CheckLanesI256 ("Avx2.ConvertToVector256Int32(sbyte)",
			               Avx2.ConvertToVector256Int32 (LoadI8 (i32SrcSbyte)), -8, -7, -6, -5, -4,
			               -3, -2, -1);

			short[] i32SrcShort = new short[8];
			for (int i = 0; i < 8; i++) i32SrcShort [i] = (short) (i * 10);
			CheckLanesI256 ("Avx2.ConvertToVector256Int32(short)",
			               Avx2.ConvertToVector256Int32 (LoadI16 (i32SrcShort)), 0, 10, 20, 30, 40,
			               50, 60, 70);

			byte[] u32SrcByte = new byte[16];
			for (int i = 0; i < 16; i++) u32SrcByte [i] = (byte) (i + 1);
			uint[] u32Result = ToArrayU256 (Avx2.ConvertToVector256UInt32 (LoadU8 (u32SrcByte)));
			bool u32Ok = true;
			for (int i = 0; i < 8; i++)
				u32Ok &= u32Result [i] == (uint) (i + 1);
			Check ("Avx2.ConvertToVector256UInt32(byte)", u32Ok);

			long[] i64Result = ToArrayI64x256 (Avx2.ConvertToVector256Int64 (LoadI8 (i32SrcSbyte)));
			bool i64Ok = true;
			for (int i = 0; i < 4; i++)
				i64Ok &= i64Result [i] == i - 8;
			Check ("Avx2.ConvertToVector256Int64(sbyte)", i64Ok);

			ulong[] u64Result = ToArrayU64x256 (Avx2.ConvertToVector256UInt64 (LoadU8 (u32SrcByte)));
			bool u64Ok = true;
			for (int i = 0; i < 4; i++)
				u64Ok &= u64Result [i] == (ulong) (i + 1);
			Check ("Avx2.ConvertToVector256UInt64(byte)", u64Ok);

			Vector256<int> extSrc = LoadI256 (1, 2, 3, 4, 5, 6, 7, 8);
			CheckLanesI32 ("Avx2.ExtractVector128(index0)", Avx2.ExtractVector128 (extSrc, 0), 1, 2,
			              3, 4);
			CheckLanesI32 ("Avx2.ExtractVector128(index1)", Avx2.ExtractVector128 (extSrc, 1), 5, 6,
			              7, 8);

			int[] extStoreBuf = new int[4];
			fixed (int* extStorePtr = extStoreBuf)
				Avx2.ExtractVector128 (extStorePtr, extSrc, 1);
			Check ("Avx2.ExtractVector128(store)",
			      extStoreBuf [0] == 5 && extStoreBuf [1] == 6 && extStoreBuf [2] == 7
			      && extStoreBuf [3] == 8);

			Vector128<int> insData = LoadI32 (100, 200, 300, 400);
			CheckLanesI256 ("Avx2.InsertVector128(reg)", Avx2.InsertVector128 (extSrc, insData, 1),
			               1, 2, 3, 4, 100, 200, 300, 400);

			fixed (int* insLoadPtr = extStoreBuf)
				CheckLanesI256 ("Avx2.InsertVector128(load)",
				               Avx2.InsertVector128 (extSrc, insLoadPtr, 0), 5, 6, 7, 8, 5, 6, 7, 8);

			int[] gatherData = { 10, 20, 30, 40, 50, 60, 70, 80 };
			fixed (int* gatherPtr = gatherData) {
				Vector128<int> gatherIdx128 = LoadI32 (0, 2, 4, 6);
				CheckLanesI32 ("Avx2.GatherVector128", Avx2.GatherVector128 (gatherPtr, gatherIdx128,
				                                                            4), 10, 30, 50, 70);

				Vector256<int> gatherIdx256 = LoadI256 (0, 1, 2, 3, 4, 5, 6, 7);
				CheckLanesI256 ("Avx2.GatherVector256",
				               Avx2.GatherVector256 (gatherPtr, gatherIdx256, 4), 10, 20, 30, 40, 50,
				               60, 70, 80);

				Vector128<int> gatherMask = LoadI32 (-1, 0, -1, 0);
				Vector128<int> gatherSrc = LoadI32 (999, 999, 999, 999);
				CheckLanesI32 ("Avx2.GatherMaskVector128",
				              Avx2.GatherMaskVector128 (gatherSrc, gatherPtr, gatherIdx128,
				                                        gatherMask, 4), 10, 999, 50, 999);
			}

			int[] mlData = { 1, 2, 3, 4, 5, 6, 7, 8 };
			fixed (int* mlPtr = mlData) {
				Vector256<int> mlMask = LoadI256 (-1, 0, -1, 0, -1, 0, -1, 0);
				CheckLanesI256 ("Avx2.MaskLoad", Avx2.MaskLoad (mlPtr, mlMask), 1, 0, 3, 0, 5, 0, 7,
				               0);
			}

			int[] msBuf = new int[8];
			fixed (int* msPtr = msBuf) {
				Vector256<int> msMask = LoadI256 (-1, 0, -1, 0, -1, 0, -1, 0);
				Vector256<int> msSource = LoadI256 (11, 22, 33, 44, 55, 66, 77, 88);
				Avx2.MaskStore (msPtr, msMask, msSource);
			}
			Check ("Avx2.MaskStore",
			      msBuf [0] == 11 && msBuf [1] == 0 && msBuf [2] == 33 && msBuf [3] == 0
			      && msBuf [4] == 55 && msBuf [5] == 0 && msBuf [6] == 77 && msBuf [7] == 0);

			Vector256<int> maxA = LoadI256 (1, -2, 3, -4, 5, -6, 7, -8);
			Vector256<int> maxB = LoadI256 (0, 0, 0, 0, 0, 0, 0, 0);
			CheckLanesI256 ("Avx2.Max", Avx2.Max (maxA, maxB), 1, 0, 3, 0, 5, 0, 7, 0);
			CheckLanesI256 ("Avx2.Min", Avx2.Min (maxA, maxB), 0, -2, 0, -4, 0, -6, 0, -8);

			sbyte[] mmSrc = new sbyte[32];
			for (int i = 0; i < 32; i++) mmSrc [i] = (sbyte) (i % 2 == 0 ? 1 : -1);
			int mmResult = Avx2.MoveMask (LoadI8x256 (mmSrc));
			int mmExpected = 0;
			for (int i = 0; i < 32; i++)
				if (i % 2 == 1) mmExpected |= 1 << i;
			Check ("Avx2.MoveMask", mmResult == mmExpected);

			byte[] msadSrc = new byte[32];
			for (int i = 0; i < 32; i++) msadSrc [i] = 5;
			ushort[] msadResult = ToArrayU16x256 (
				Avx2.MultipleSumAbsoluteDifferences (LoadU8x256 (msadSrc), LoadU8x256 (msadSrc), 0));
			bool msadOk = true;
			for (int i = 0; i < 16; i++)
				msadOk &= msadResult [i] == 0;
			Check ("Avx2.MultipleSumAbsoluteDifferences(equal constant inputs is zero)", msadOk);

			Vector256<int> mulWA = LoadI256 (2, 0, 3, 0, 4, 0, 5, 0);
			Vector256<int> mulWB = LoadI256 (10, 0, 10, 0, 10, 0, 10, 0);
			long[] mulWResult = ToArrayI64x256 (Avx2.Multiply (mulWA, mulWB));
			Check ("Avx2.Multiply(int->long)",
			      mulWResult [0] == 20 && mulWResult [1] == 30 && mulWResult [2] == 40
			      && mulWResult [3] == 50);

			Vector256<uint> mulWUA = LoadU256 (2, 0, 3, 0, 4, 0, 5, 0);
			Vector256<uint> mulWUB = LoadU256 (10, 0, 10, 0, 10, 0, 10, 0);
			ulong[] mulWUResult = ToArrayU64x256 (Avx2.Multiply (mulWUA, mulWUB));
			Check ("Avx2.Multiply(uint->ulong)",
			      mulWUResult [0] == 20 && mulWUResult [1] == 30 && mulWUResult [2] == 40
			      && mulWUResult [3] == 50);

			short[] mulHiA = new short[16];
			short[] mulHiB = new short[16];
			for (int i = 0; i < 16; i++) { mulHiA [i] = 30000; mulHiB [i] = 30000; }
			short[] mulHiResult =
				ToArrayI16x256 (Avx2.MultiplyHigh (LoadI16x256 (mulHiA), LoadI16x256 (mulHiB)));
			bool mulHiOk = true;
			for (int i = 0; i < 16; i++)
				mulHiOk &= mulHiResult [i] == (short) ((30000 * 30000) >> 16);
			Check ("Avx2.MultiplyHigh", mulHiOk);

			ushort[] mulHiUA = new ushort[16];
			ushort[] mulHiUB = new ushort[16];
			for (int i = 0; i < 16; i++) { mulHiUA [i] = 60000; mulHiUB [i] = 60000; }
			ushort[] mulHiUResult =
				ToArrayU16x256 (Avx2.MultiplyHigh (LoadU16x256 (mulHiUA), LoadU16x256 (mulHiUB)));
			ushort expectedHiU = (ushort) ((60000u * 60000u) >> 16);
			bool mulHiUOk = true;
			for (int i = 0; i < 16; i++)
				mulHiUOk &= mulHiUResult [i] == expectedHiU;
			Check ("Avx2.MultiplyHigh(ushort)", mulHiUOk);

			short[] mhrsA = new short[16];
			short[] mhrsB = new short[16];
			for (int i = 0; i < 16; i++) { mhrsA [i] = 0; mhrsB [i] = 12345; }
			short[] mhrsResult =
				ToArrayI16x256 (Avx2.MultiplyHighRoundScale (LoadI16x256 (mhrsA), LoadI16x256 (mhrsB)));
			bool mhrsOk = true;
			for (int i = 0; i < 16; i++)
				mhrsOk &= mhrsResult [i] == 0;
			Check ("Avx2.MultiplyHighRoundScale(zero operand is zero)", mhrsOk);

			short[] mlowA = new short[16];
			short[] mlowB = new short[16];
			for (int i = 0; i < 16; i++) { mlowA [i] = (short) (i + 1); mlowB [i] = 3; }
			short[] mlowResult =
				ToArrayI16x256 (Avx2.MultiplyLow (LoadI16x256 (mlowA), LoadI16x256 (mlowB)));
			bool mlowOk = true;
			for (int i = 0; i < 16; i++)
				mlowOk &= mlowResult [i] == (short) (3 * (i + 1));
			Check ("Avx2.MultiplyLow(short)", mlowOk);

			Vector256<int> mlowIntA = LoadI256 (1, 2, 3, 4, 5, 6, 7, 8);
			Vector256<int> mlowIntB = LoadI256 (3, 3, 3, 3, 3, 3, 3, 3);
			CheckLanesI256 ("Avx2.MultiplyLow(int)", Avx2.MultiplyLow (mlowIntA, mlowIntB), 3, 6, 9,
			               12, 15, 18, 21, 24);

			short[] pmaddA = new short[16];
			short[] pmaddB = new short[16];
			for (int i = 0; i < 16; i++) { pmaddA [i] = (short) (i + 1); pmaddB [i] = 2; }
			int[] pmaddResult =
				ToArrayI256 (Avx2.MultiplyAddAdjacent (LoadI16x256 (pmaddA), LoadI16x256 (pmaddB)));
			bool pmaddOk = true;
			for (int i = 0; i < 8; i++)
				pmaddOk &= pmaddResult [i] == (2 * i + 1) * 2 + (2 * i + 2) * 2;
			Check ("Avx2.MultiplyAddAdjacent(short)", pmaddOk);

			byte[] pmaddUA = new byte[32];
			sbyte[] pmaddSB = new sbyte[32];
			for (int i = 0; i < 32; i++) {
				pmaddUA [i] = 10;
				pmaddSB [i] = (sbyte) (i % 2 == 0 ? 1 : -1);
			}
			short[] pmaddUResult = ToArrayI16x256 (
				Avx2.MultiplyAddAdjacent (LoadU8x256 (pmaddUA), LoadI8x256 (pmaddSB)));
			bool pmaddUOk = true;
			for (int i = 0; i < 16; i++)
				pmaddUOk &= pmaddUResult [i] == 0;
			Check ("Avx2.MultiplyAddAdjacent(byte,sbyte)", pmaddUOk);

			short[] packSA = new short[16];
			short[] packSB = new short[16];
			for (int i = 0; i < 16; i++) { packSA [i] = 200; packSB [i] = -200; }
			sbyte[] packSResult =
				ToArrayI8x256 (Avx2.PackSignedSaturate (LoadI16x256 (packSA), LoadI16x256 (packSB)));
			bool packSOk = true;
			for (int i = 0; i < 32; i++)
				packSOk &= packSResult [i] == (sbyte) (i % 16 < 8 ? 127 : -128);
			Check ("Avx2.PackSignedSaturate(short->sbyte)", packSOk);

			Vector256<int> packDA = LoadI256 (40000, 40000, 40000, 40000, 40000, 40000, 40000, 40000);
			Vector256<int> packDB =
				LoadI256 (-40000, -40000, -40000, -40000, -40000, -40000, -40000, -40000);
			short[] packDResult = ToArrayI16x256 (Avx2.PackSignedSaturate (packDA, packDB));
			bool packDOk = true;
			for (int i = 0; i < 16; i++)
				packDOk &= packDResult [i] == (short) (i % 8 < 4 ? 32767 : -32768);
			Check ("Avx2.PackSignedSaturate(int->short)", packDOk);

			short[] packUSA = new short[16];
			short[] packUSB = new short[16];
			for (int i = 0; i < 16; i++) { packUSA [i] = 300; packUSB [i] = -50; }
			byte[] packUSResult = ToArrayU8x256 (
				Avx2.PackUnsignedSaturate (LoadI16x256 (packUSA), LoadI16x256 (packUSB)));
			bool packUSOk = true;
			for (int i = 0; i < 32; i++)
				packUSOk &= packUSResult [i] == (byte) (i % 16 < 8 ? 255 : 0);
			Check ("Avx2.PackUnsignedSaturate(short->byte)", packUSOk);

			Vector256<int> packUDA = LoadI256 (70000, 70000, 70000, 70000, 70000, 70000, 70000, 70000);
			Vector256<int> packUDB = LoadI256 (-10, -10, -10, -10, -10, -10, -10, -10);
			ushort[] packUDResult = ToArrayU16x256 (Avx2.PackUnsignedSaturate (packUDA, packUDB));
			bool packUDOk = true;
			for (int i = 0; i < 16; i++)
				packUDOk &= packUDResult [i] == (ushort) (i % 8 < 4 ? 65535 : 0);
			Check ("Avx2.PackUnsignedSaturate(int->ushort)", packUDOk);

			Vector256<int> permLeft = LoadI256 (1, 2, 3, 4, 5, 6, 7, 8);
			Vector256<int> permRight = LoadI256 (10, 20, 30, 40, 50, 60, 70, 80);
			CheckLanesI256 ("Avx2.Permute2x128", Avx2.Permute2x128 (permLeft, permRight, 0x20), 1, 2,
			               3, 4, 10, 20, 30, 40);

			Vector256<long> permQ = LoadI64x256 (10, 20, 30, 40);
			long[] permQResult = ToArrayI64x256 (Avx2.Permute4x64 (permQ, 0x1B));
			Check ("Avx2.Permute4x64",
			      permQResult [0] == 40 && permQResult [1] == 30 && permQResult [2] == 20
			      && permQResult [3] == 10);

			Vector256<int> pvSrc = LoadI256 (10, 20, 30, 40, 50, 60, 70, 80);
			Vector256<int> pvIdx = LoadI256 (7, 6, 5, 4, 3, 2, 1, 0);
			CheckLanesI256 ("Avx2.PermuteVar8x32(int)", Avx2.PermuteVar8x32 (pvSrc, pvIdx), 80, 70,
			               60, 50, 40, 30, 20, 10);

			Vector256<float> pvSrcF = LoadF256 (1f, 2f, 3f, 4f, 5f, 6f, 7f, 8f);
			CheckLanesF256 ("Avx2.PermuteVar8x32(float)", Avx2.PermuteVar8x32 (pvSrcF, pvIdx), 8f, 7f,
			               6f, 5f, 4f, 3f, 2f, 1f);

			Vector256<int> shiftSrc = LoadI256 (1, 2, 3, 4, 5, 6, 7, 8);
			CheckLanesI256 ("Avx2.ShiftLeftLogical(imm)", Avx2.ShiftLeftLogical (shiftSrc, (byte) 2),
			               4, 8, 12, 16, 20, 24, 28, 32);
			CheckLanesI256 ("Avx2.ShiftRightLogical(imm)",
			               Avx2.ShiftRightLogical (shiftSrc, (byte) 1), 0, 1, 1, 2, 2, 3, 3, 4);

			Vector256<int> shiftNeg = LoadI256 (-8, -4, -2, -1, 8, 4, 2, 1);
			CheckLanesI256 ("Avx2.ShiftRightArithmetic(imm)",
			               Avx2.ShiftRightArithmetic (shiftNeg, (byte) 1), -4, -2, -1, -1, 4, 2, 1,
			               0);

			Vector128<int> shiftCount = LoadI32 (1, 0, 0, 0);
			CheckLanesI256 ("Avx2.ShiftLeftLogical(count)",
			               Avx2.ShiftLeftLogical (shiftSrc, shiftCount), 2, 4, 6, 8, 10, 12, 14, 16);
			CheckLanesI256 ("Avx2.ShiftRightLogical(count)",
			               Avx2.ShiftRightLogical (shiftSrc, shiftCount), 0, 1, 1, 2, 2, 3, 3, 4);
			CheckLanesI256 ("Avx2.ShiftRightArithmetic(count)",
			               Avx2.ShiftRightArithmetic (shiftNeg, shiftCount), -4, -2, -1, -1, 4, 2, 1,
			               0);

			byte[] laneShiftSrc = new byte[32];
			for (int i = 0; i < 32; i++) laneShiftSrc [i] = (byte) (i % 16 + 1);
			byte[] laneShiftLeftResult =
				ToArrayU8x256 (Avx2.ShiftLeftLogical128BitLane (LoadU8x256 (laneShiftSrc), 1));
			bool laneLeftOk = true;
			for (int h = 0; h < 2; h++) {
				laneLeftOk &= laneShiftLeftResult [h * 16] == 0;
				for (int i = 1; i < 16; i++)
					laneLeftOk &= laneShiftLeftResult [h * 16 + i] == laneShiftSrc [h * 16 + i - 1];
			}
			Check ("Avx2.ShiftLeftLogical128BitLane", laneLeftOk);

			byte[] laneShiftRightResult =
				ToArrayU8x256 (Avx2.ShiftRightLogical128BitLane (LoadU8x256 (laneShiftSrc), 1));
			bool laneRightOk = true;
			for (int h = 0; h < 2; h++) {
				for (int i = 0; i < 15; i++)
					laneRightOk &= laneShiftRightResult [h * 16 + i] == laneShiftSrc [h * 16 + i + 1];
				laneRightOk &= laneShiftRightResult [h * 16 + 15] == 0;
			}
			Check ("Avx2.ShiftRightLogical128BitLane", laneRightOk);

			Vector256<uint> shiftVarCounts = LoadU256 (0, 1, 2, 3, 4, 5, 6, 7);
			Vector256<int> shiftVarBase = LoadI256 (1, 1, 1, 1, 1, 1, 1, 1);
			CheckLanesI256 ("Avx2.ShiftLeftLogicalVariable",
			               Avx2.ShiftLeftLogicalVariable (shiftVarBase, shiftVarCounts), 1, 2, 4, 8,
			               16, 32, 64, 128);

			Vector256<uint> shiftVarSrc = LoadU256 (256, 256, 256, 256, 256, 256, 256, 256);
			uint[] shiftVarRightResult =
				ToArrayU256 (Avx2.ShiftRightLogicalVariable (shiftVarSrc, shiftVarCounts));
			Check ("Avx2.ShiftRightLogicalVariable",
			      shiftVarRightResult [0] == 256 && shiftVarRightResult [1] == 128
			      && shiftVarRightResult [2] == 64 && shiftVarRightResult [3] == 32
			      && shiftVarRightResult [4] == 16 && shiftVarRightResult [5] == 8
			      && shiftVarRightResult [6] == 4 && shiftVarRightResult [7] == 2);

			Vector256<int> shiftVarNeg = LoadI256 (-256, -256, -256, -256, -256, -256, -256, -256);
			CheckLanesI256 ("Avx2.ShiftRightArithmeticVariable",
			               Avx2.ShiftRightArithmeticVariable (shiftVarNeg, shiftVarCounts), -256,
			               -128, -64, -32, -16, -8, -4, -2);

			Vector128<uint> shiftVar128Counts = LoadU32 (0, 1, 2, 3);
			Vector128<int> shiftVar128Base = LoadI32 (1, 1, 1, 1);
			CheckLanesI32 ("Avx2.ShiftLeftLogicalVariable(128)",
			              Avx2.ShiftLeftLogicalVariable (shiftVar128Base, shiftVar128Counts), 1, 2,
			              4, 8);

			byte[] shufSrc = new byte[32];
			for (int i = 0; i < 32; i++) shufSrc [i] = (byte) i;
			byte[] shufMask = new byte[32];
			for (int i = 0; i < 32; i++) shufMask [i] = (byte) (15 - i % 16);
			byte[] shufResult =
				ToArrayU8x256 (Avx2.Shuffle (LoadU8x256 (shufSrc), LoadU8x256 (shufMask)));
			bool shufOk = true;
			for (int i = 0; i < 32; i++)
				shufOk &= shufResult [i] == (byte) (i / 16 * 16 + (15 - i % 16));
			Check ("Avx2.Shuffle(VV)", shufOk);

			Vector256<int> shufDSrc = LoadI256 (1, 2, 3, 4, 5, 6, 7, 8);
			CheckLanesI256 ("Avx2.Shuffle(int)", Avx2.Shuffle (shufDSrc, 0x1B), 4, 3, 2, 1, 8, 7, 6,
			               5);

			short[] shufHLSrc = new short[16];
			for (int i = 0; i < 16; i++) shufHLSrc [i] = (short) i;
			short[] shufHighResult =
				ToArrayI16x256 (Avx2.ShuffleHigh (LoadI16x256 (shufHLSrc), 0x1B));
			bool shufHighOk = true;
			for (int h = 0; h < 2; h++) {
				for (int i = 0; i < 4; i++)
					shufHighOk &= shufHighResult [h * 8 + i] == shufHLSrc [h * 8 + i];
				for (int i = 0; i < 4; i++)
					shufHighOk &=
						shufHighResult [h * 8 + 4 + i] == shufHLSrc [h * 8 + 4 + (3 - i)];
			}
			Check ("Avx2.ShuffleHigh", shufHighOk);

			short[] shufLowResult = ToArrayI16x256 (Avx2.ShuffleLow (LoadI16x256 (shufHLSrc), 0x1B));
			bool shufLowOk = true;
			for (int h = 0; h < 2; h++) {
				for (int i = 0; i < 4; i++)
					shufLowOk &= shufLowResult [h * 8 + i] == shufHLSrc [h * 8 + (3 - i)];
				for (int i = 0; i < 4; i++)
					shufLowOk &= shufLowResult [h * 8 + 4 + i] == shufHLSrc [h * 8 + 4 + i];
			}
			Check ("Avx2.ShuffleLow", shufLowOk);

			sbyte[] signCtrl8 = new sbyte[32];
			for (int i = 0; i < 32; i++)
				signCtrl8 [i] = (sbyte) (i % 3 == 0 ? -1 : i % 3 == 1 ? 0 : 1);
			sbyte[] signResult = ToArrayI8x256 (Avx2.Sign (LoadI8x256 (seq8), LoadI8x256 (signCtrl8)));
			bool signOk = true;
			for (int i = 0; i < 32; i++) {
				sbyte expected = (sbyte) (i % 3 == 0 ? -seq8 [i] : i % 3 == 1 ? 0 : seq8 [i]);
				signOk &= signResult [i] == expected;
			}
			Check ("Avx2.Sign(sbyte)", signOk);

			byte[] sadA = new byte[32];
			byte[] sadB = new byte[32];
			for (int i = 0; i < 32; i++) {
				sadA [i] = (byte) (i % 16 + 10);
				sadB [i] = (byte) (i % 16);
			}
			ushort[] sadResult =
				ToArrayU16x256 (Avx2.SumAbsoluteDifferences (LoadU8x256 (sadA), LoadU8x256 (sadB)));
			bool sadOk = true;
			for (int i = 0; i < 16; i++)
				sadOk &= sadResult [i] == (i % 4 == 0 ? 80 : 0);
			Check ("Avx2.SumAbsoluteDifferences", sadOk);

			Vector256<int> unpackA = LoadI256 (1, 2, 3, 4, 5, 6, 7, 8);
			Vector256<int> unpackB = LoadI256 (10, 20, 30, 40, 50, 60, 70, 80);
			CheckLanesI256 ("Avx2.UnpackLow", Avx2.UnpackLow (unpackA, unpackB), 1, 10, 2, 20, 5, 50,
			               6, 60);
			CheckLanesI256 ("Avx2.UnpackHigh", Avx2.UnpackHigh (unpackA, unpackB), 3, 30, 4, 40, 7,
			               70, 8, 80);
		}

		Check ("Popcnt.IsSupported", Popcnt.IsSupported);

		if (Popcnt.IsSupported) {
			Check ("Popcnt.PopCount(uint) zero", Popcnt.PopCount (0u) == 0);
			Check ("Popcnt.PopCount(uint) all ones", Popcnt.PopCount (0xFFFFFFFFu) == 32);
			Check ("Popcnt.PopCount(uint)", Popcnt.PopCount (0b1011u) == 3);
			Check ("Popcnt.PopCount(ulong) zero", Popcnt.PopCount (0ul) == 0);
			Check ("Popcnt.PopCount(ulong) all ones", Popcnt.PopCount (0xFFFFFFFFFFFFFFFFul) == 64);
			Check ("Popcnt.PopCount(ulong)", Popcnt.PopCount (0x8000000000000001ul) == 2);
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
