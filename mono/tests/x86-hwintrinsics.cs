/* Tests LLVM lowering for System.Runtime.Intrinsics.X86.Sse, Sse2, Sse3, Ssse3, Sse41 and Sse42. */
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
				int* np = stackalloc int[4] { 9, 8, 7, 6 };
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
