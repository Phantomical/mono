// Differential gate for a SIMD lowering the backend does not have yet. Today
// every Mono.Simd, System.Numerics.Vector4 and System.Numerics.Vector<T>
// operation is ordinary managed IL, so the interpreter and both compiled
// tiers run the same instructions and agree trivially. This program freezes
// that agreement so a later change that replaces those bodies with LLVM
// vector IR has something to fail against.
//
// Each operation is computed four ways: interpreted, promoted to tier 1,
// promoted to tier 2, and through a delegate created over the operation's
// own compiled body (entering its thunk rather than a copy folded into a
// caller) at tier 1 and again at tier 2. All four are compared against the
// interpreted result as raw bytes, never with ==, because a float compare
// treats a NaN as unequal to itself and would hide the one difference a bit
// pattern change is meant to catch.

using System;
using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using Mono.Simd;

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class SimdSemantics
{
	const int TIER1 = 1;
	const int TIER2 = 2;
	const int ROWS = 14;

	static int comparisons;
	static int mismatches;
	static int promoteFailures;

	// ==================== edge tables ====================

	static readonly float[] FE = {
		0f, -0f, 1f, -1f, 0.5f, float.Epsilon, float.MaxValue, float.MinValue,
		float.PositiveInfinity, float.NegativeInfinity,
		BitConverter.Int32BitsToSingle (unchecked ((int) 0x7FC00000)),
		BitConverter.Int32BitsToSingle (unchecked ((int) 0xFFC00000)),
		3f, 7f
	};

	static readonly double[] DE = {
		0d, -0d, 1d, -1d, 0.5d, double.Epsilon, double.MaxValue, double.MinValue,
		double.PositiveInfinity, double.NegativeInfinity,
		BitConverter.Int64BitsToDouble (unchecked ((long) 0x7FF8000000000000L)),
		BitConverter.Int64BitsToDouble (unchecked ((long) 0xFFF8000000000000L)),
		3d, 7d
	};

	static readonly int[] IE = { 0, 1, -1, int.MinValue, int.MaxValue, 100 };
	static readonly uint[] UIE = { 0u, 1u, uint.MaxValue, 0x80000000u, 100u };
	static readonly long[] LE = { 0L, 1L, -1L, long.MinValue, long.MaxValue, 100L };
	static readonly ulong[] ULE = { 0UL, 1UL, ulong.MaxValue, 0x8000000000000000UL, 100UL };
	static readonly short[] SE = { 0, 1, -1, short.MinValue, short.MaxValue, 100 };
	static readonly ushort[] USE = { 0, 1, ushort.MaxValue, 0x8000, 100 };
	static readonly byte[] BE = { 0, 1, 255, 0x7F, 0x80, 100 };
	static readonly sbyte[] SBE = { 0, 1, -1, sbyte.MinValue, sbyte.MaxValue, 100 };

	// Abs (int.MinValue) has no representable positive result and
	// Math.Abs () throws OverflowException for it, so an Abs check reads
	// from a table with no MinValue in it rather than the shared one above.
	static readonly int[] IEabs = { 0, 1, -1, int.MaxValue, 100 };
	static readonly long[] LEabs = { 0L, 1L, -1L, long.MaxValue, 100L };
	static readonly short[] SEabs = { 0, 1, -1, short.MaxValue, 100 };
	static readonly sbyte[] SBEabs = { 0, 1, -1, sbyte.MaxValue, 100 };
	static readonly int[] ShAmt = { 0, 1, 7, 31, 32, 33, 63, 64, -1 };

	// ==================== byte reinterpretation ====================
	//
	// Every comparison in this file reads the memory image of a result
	// rather than any one field, so a lane the test did not think to name
	// is still covered.

	static unsafe byte[] Bytes (Vector4f v) { byte[] r = new byte[sizeof (Vector4f)]; fixed (byte* p = r) *(Vector4f*) p = v; return r; }
	static unsafe byte[] Bytes (Vector4i v) { byte[] r = new byte[sizeof (Vector4i)]; fixed (byte* p = r) *(Vector4i*) p = v; return r; }
	static unsafe byte[] Bytes (Vector4ui v) { byte[] r = new byte[sizeof (Vector4ui)]; fixed (byte* p = r) *(Vector4ui*) p = v; return r; }
	static unsafe byte[] Bytes (Vector2d v) { byte[] r = new byte[sizeof (Vector2d)]; fixed (byte* p = r) *(Vector2d*) p = v; return r; }
	static unsafe byte[] Bytes (Vector2l v) { byte[] r = new byte[sizeof (Vector2l)]; fixed (byte* p = r) *(Vector2l*) p = v; return r; }
	static unsafe byte[] Bytes (Vector2ul v) { byte[] r = new byte[sizeof (Vector2ul)]; fixed (byte* p = r) *(Vector2ul*) p = v; return r; }
	static unsafe byte[] Bytes (Vector8s v) { byte[] r = new byte[sizeof (Vector8s)]; fixed (byte* p = r) *(Vector8s*) p = v; return r; }
	static unsafe byte[] Bytes (Vector8us v) { byte[] r = new byte[sizeof (Vector8us)]; fixed (byte* p = r) *(Vector8us*) p = v; return r; }
	static unsafe byte[] Bytes (Vector16b v) { byte[] r = new byte[sizeof (Vector16b)]; fixed (byte* p = r) *(Vector16b*) p = v; return r; }
	static unsafe byte[] Bytes (Vector16sb v) { byte[] r = new byte[sizeof (Vector16sb)]; fixed (byte* p = r) *(Vector16sb*) p = v; return r; }
	static unsafe byte[] Bytes (System.Numerics.Vector4 v) { byte[] r = new byte[sizeof (System.Numerics.Vector4)]; fixed (byte* p = r) *(System.Numerics.Vector4*) p = v; return r; }

	static byte[] Bytes<T> (Vector<T> v) where T : struct
	{
		byte[] r = new byte[System.Runtime.InteropServices.Marshal.SizeOf (typeof (Vector<T>))];
		System.Runtime.InteropServices.GCHandle h = System.Runtime.InteropServices.GCHandle.Alloc (r, System.Runtime.InteropServices.GCHandleType.Pinned);
		try {
			System.Runtime.InteropServices.Marshal.StructureToPtr (v, h.AddrOfPinnedObject (), false);
		} finally {
			h.Free ();
		}
		return r;
	}

	// ==================== reflection helpers ====================

	static MethodInfo Kernel (string name)
	{
		MethodInfo m = typeof (SimdSemantics).GetMethod (name, BindingFlags.NonPublic | BindingFlags.Static);
		if (m == null)
			throw new Exception ("no such kernel: " + name);
		return m;
	}

	static void Promote (MethodInfo m, int tier)
	{
		if (!Mono.Tiering.MonoTier.PromoteNow (m.MethodHandle.Value, tier)) {
			Console.WriteLine ("PromoteNow FAILED for " + m + " at tier " + tier);
			promoteFailures++;
		}
	}

	static void Promote (string kernelName, int tier)
	{
		Promote (Kernel (kernelName), tier);
	}

	static MethodInfo Explicit (Type from, Type to)
	{
		foreach (MethodInfo m in from.GetMethods (BindingFlags.Public | BindingFlags.Static)) {
			if (m.Name != "op_Explicit")
				continue;
			ParameterInfo[] p = m.GetParameters ();
			if (p.Length == 1 && p[0].ParameterType == from && m.ReturnType == to)
				return m;
		}
		throw new Exception ("no explicit conversion " + from + " -> " + to);
	}

	static MethodInfo VecGeneric (string name, Type t)
	{
		foreach (MethodInfo m in typeof (System.Numerics.Vector).GetMethods (BindingFlags.Public | BindingFlags.Static)) {
			if (m.Name == name && m.IsGenericMethodDefinition && m.GetGenericArguments ().Length == 1)
				return m.MakeGenericMethod (t);
		}
		throw new Exception ("no generic Vector." + name);
	}

	static byte[] Try (Func<byte[]> f)
	{
		try {
			return f ();
		} catch (DivideByZeroException) {
			return null;
		}
	}

	// ==================== reporting ====================

	static string Hex (byte[] b, int off, int len)
	{
		var sb = new System.Text.StringBuilder ();
		for (int i = len - 1; i >= 0; i--)
			sb.Append (b[off + i].ToString ("X2"));
		return sb.ToString ();
	}

	static readonly string[] KernelArms = { "tier1", "tier2" };
	static readonly string[] AllArms = { "tier1", "tier2", "delegate-tier1", "delegate-tier2" };

	// ECMA-335 I.12.1.3 makes any NaN payload the answer an ordinary float
	// operation gives, and a multi-term sum can settle on a different one
	// depending on the order two NaNs are combined in - already true of
	// Vector4.Dot, .Distance and .Normalize between an inlined and a
	// standalone compile of the same IL, with nothing SIMD-specific in it.
	// floatRelax accepts either NaN payload for those, while every non-NaN
	// bit and every other operation still has to match exactly.
	static bool IsNaNLane (byte[] b, int off, int len)
	{
		if (len == 4)
			return float.IsNaN (BitConverter.ToSingle (b, off));
		if (len == 8)
			return double.IsNaN (BitConverter.ToDouble (b, off));
		return false;
	}

	static void Report (string family, string op, int row, byte[] baseline, string[] armNames, byte[][] arms, int elemSize, bool floatRelax = false)
	{
		for (int a = 0; a < arms.Length; a++) {
			comparisons++;
			byte[] cur = arms[a];

			if (baseline == null || cur == null) {
				if (baseline != cur) {
					mismatches++;
					Console.WriteLine (family + " " + op + " row" + row + ": interp=" +
						(baseline == null ? "threw" : "value") + " " + armNames[a] + "=" +
						(cur == null ? "threw" : "value"));
				}
				continue;
			}

			if (baseline.Length != cur.Length) {
				mismatches++;
				Console.WriteLine (family + " " + op + " row" + row + " " + armNames[a] + ": size mismatch");
				continue;
			}

			for (int i = 0; i < baseline.Length; i += elemSize) {
				bool diff = false;
				for (int j = 0; j < elemSize; j++) {
					if (baseline[i + j] != cur[i + j]) {
						diff = true;
						break;
					}
				}
				if (diff && floatRelax && IsNaNLane (baseline, i, elemSize) && IsNaNLane (cur, i, elemSize))
					diff = false;

				if (diff) {
					mismatches++;
					Console.WriteLine (family + " " + op + " row" + row + " lane" + (i / elemSize) +
						" " + armNames[a] + ": interp=0x" + Hex (baseline, i, elemSize) +
						" " + armNames[a] + "=0x" + Hex (cur, i, elemSize));
				}
			}
		}
	}

	// ==================== drivers ====================

	static void RunKernelOnly (string family, string op, string kernelName, Func<int, byte[]> kernel, int elemSize)
	{
		var interp = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			interp[r] = kernel (r);

		Promote (kernelName, TIER1);
		var t1 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			t1[r] = kernel (r);

		Promote (kernelName, TIER2);
		var t2 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			t2[r] = kernel (r);

		for (int r = 0; r < ROWS; r++)
			Report (family, op, r, interp[r], KernelArms, new[] { t1[r], t2[r] }, elemSize);
	}

	static void RunBinary<TA, TB, TR> (string family, string op, string kernelName, Func<int, byte[]> kernel,
	                                    MethodInfo body, TA[] a, TB[] b, Func<TR, byte[]> toBytes, int elemSize, bool floatRelax = false)
	{
		var interp = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			interp[r] = kernel (r);

		Promote (kernelName, TIER1);
		var t1 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			t1[r] = kernel (r);

		Promote (kernelName, TIER2);
		var t2 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			t2[r] = kernel (r);

		var del = (Func<TA, TB, TR>) Delegate.CreateDelegate (typeof (Func<TA, TB, TR>), body);

		Promote (body, TIER1);
		var d1 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			d1[r] = toBytes (del (a[r % a.Length], b[r % b.Length]));

		Promote (body, TIER2);
		var d2 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			d2[r] = toBytes (del (a[r % a.Length], b[r % b.Length]));

		for (int r = 0; r < ROWS; r++)
			Report (family, op, r, interp[r], AllArms, new[] { t1[r], t2[r], d1[r], d2[r] }, elemSize, floatRelax);
	}

	static void RunUnary<TA, TR> (string family, string op, string kernelName, Func<int, byte[]> kernel,
	                               MethodInfo body, TA[] a, Func<TR, byte[]> toBytes, int elemSize, bool floatRelax = false)
	{
		var interp = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			interp[r] = kernel (r);

		Promote (kernelName, TIER1);
		var t1 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			t1[r] = kernel (r);

		Promote (kernelName, TIER2);
		var t2 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			t2[r] = kernel (r);

		var del = (Func<TA, TR>) Delegate.CreateDelegate (typeof (Func<TA, TR>), body);

		Promote (body, TIER1);
		var d1 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			d1[r] = toBytes (del (a[r % a.Length]));

		Promote (body, TIER2);
		var d2 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			d2[r] = toBytes (del (a[r % a.Length]));

		for (int r = 0; r < ROWS; r++)
			Report (family, op, r, interp[r], AllArms, new[] { t1[r], t2[r], d1[r], d2[r] }, elemSize, floatRelax);
	}

	static void RunTernary<TA, TB, TC, TR> (string family, string op, string kernelName, Func<int, byte[]> kernel,
	                                         MethodInfo body, TA[] a, TB[] b, TC c, Func<TR, byte[]> toBytes, int elemSize)
	{
		var interp = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			interp[r] = kernel (r);

		Promote (kernelName, TIER1);
		var t1 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			t1[r] = kernel (r);

		Promote (kernelName, TIER2);
		var t2 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			t2[r] = kernel (r);

		var del = (Func<TA, TB, TC, TR>) Delegate.CreateDelegate (typeof (Func<TA, TB, TC, TR>), body);

		Promote (body, TIER1);
		var d1 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			d1[r] = toBytes (del (a[r % a.Length], b[r % b.Length], c));

		Promote (body, TIER2);
		var d2 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++)
			d2[r] = toBytes (del (a[r % a.Length], b[r % b.Length], c));

		for (int r = 0; r < ROWS; r++)
			Report (family, op, r, interp[r], AllArms, new[] { t1[r], t2[r], d1[r], d2[r] }, elemSize);
	}

	// ==================== row construction ====================

	static Vector4f[] MakeV4f (float[] e, int shift)
	{
		var r = new Vector4f[ROWS];
		for (int i = 0; i < ROWS; i++)
			r[i] = new Vector4f (e[(i + shift) % e.Length], e[(i + shift + 1) % e.Length],
			                      e[(i + shift + 2) % e.Length], e[(i + shift + 3) % e.Length]);
		return r;
	}

	static Vector4i[] MakeV4i (int[] e, int shift)
	{
		var r = new Vector4i[ROWS];
		for (int i = 0; i < ROWS; i++)
			r[i] = new Vector4i (e[(i + shift) % e.Length], e[(i + shift + 1) % e.Length],
			                      e[(i + shift + 2) % e.Length], e[(i + shift + 3) % e.Length]);
		return r;
	}

	static Vector4ui[] MakeV4ui (uint[] e, int shift)
	{
		var r = new Vector4ui[ROWS];
		for (int i = 0; i < ROWS; i++)
			r[i] = new Vector4ui (e[(i + shift) % e.Length], e[(i + shift + 1) % e.Length],
			                       e[(i + shift + 2) % e.Length], e[(i + shift + 3) % e.Length]);
		return r;
	}

	static Vector2d[] MakeV2d (double[] e, int shift)
	{
		var r = new Vector2d[ROWS];
		for (int i = 0; i < ROWS; i++)
			r[i] = new Vector2d (e[(i + shift) % e.Length], e[(i + shift + 1) % e.Length]);
		return r;
	}

	static Vector2l[] MakeV2l (long[] e, int shift)
	{
		var r = new Vector2l[ROWS];
		for (int i = 0; i < ROWS; i++)
			r[i] = new Vector2l (e[(i + shift) % e.Length], e[(i + shift + 1) % e.Length]);
		return r;
	}

	static Vector2ul[] MakeV2ul (ulong[] e, int shift)
	{
		var r = new Vector2ul[ROWS];
		for (int i = 0; i < ROWS; i++)
			r[i] = new Vector2ul (e[(i + shift) % e.Length], e[(i + shift + 1) % e.Length]);
		return r;
	}

	static Vector8s[] MakeV8s (short[] e, int shift)
	{
		var r = new Vector8s[ROWS];
		for (int i = 0; i < ROWS; i++)
			r[i] = new Vector8s (e[(i + shift) % e.Length], e[(i + shift + 1) % e.Length], e[(i + shift + 2) % e.Length],
			                      e[(i + shift + 3) % e.Length], e[(i + shift + 4) % e.Length], e[(i + shift + 5) % e.Length],
			                      e[(i + shift + 6) % e.Length], e[(i + shift + 7) % e.Length]);
		return r;
	}

	static Vector8us[] MakeV8us (ushort[] e, int shift)
	{
		var r = new Vector8us[ROWS];
		for (int i = 0; i < ROWS; i++)
			r[i] = new Vector8us (e[(i + shift) % e.Length], e[(i + shift + 1) % e.Length], e[(i + shift + 2) % e.Length],
			                       e[(i + shift + 3) % e.Length], e[(i + shift + 4) % e.Length], e[(i + shift + 5) % e.Length],
			                       e[(i + shift + 6) % e.Length], e[(i + shift + 7) % e.Length]);
		return r;
	}

	static Vector16b[] MakeV16b (byte[] e, int shift)
	{
		var r = new Vector16b[ROWS];
		for (int i = 0; i < ROWS; i++)
			r[i] = new Vector16b (e[(i + shift) % e.Length], e[(i + shift + 1) % e.Length], e[(i + shift + 2) % e.Length],
			                       e[(i + shift + 3) % e.Length], e[(i + shift + 4) % e.Length], e[(i + shift + 5) % e.Length],
			                       e[(i + shift + 6) % e.Length], e[(i + shift + 7) % e.Length], e[(i + shift + 8) % e.Length],
			                       e[(i + shift + 9) % e.Length], e[(i + shift + 10) % e.Length], e[(i + shift + 11) % e.Length],
			                       e[(i + shift + 12) % e.Length], e[(i + shift + 13) % e.Length], e[(i + shift + 14) % e.Length],
			                       e[(i + shift + 15) % e.Length]);
		return r;
	}

	static Vector16sb[] MakeV16sb (sbyte[] e, int shift)
	{
		var r = new Vector16sb[ROWS];
		for (int i = 0; i < ROWS; i++)
			r[i] = new Vector16sb (e[(i + shift) % e.Length], e[(i + shift + 1) % e.Length], e[(i + shift + 2) % e.Length],
			                        e[(i + shift + 3) % e.Length], e[(i + shift + 4) % e.Length], e[(i + shift + 5) % e.Length],
			                        e[(i + shift + 6) % e.Length], e[(i + shift + 7) % e.Length], e[(i + shift + 8) % e.Length],
			                        e[(i + shift + 9) % e.Length], e[(i + shift + 10) % e.Length], e[(i + shift + 11) % e.Length],
			                        e[(i + shift + 12) % e.Length], e[(i + shift + 13) % e.Length], e[(i + shift + 14) % e.Length],
			                        e[(i + shift + 15) % e.Length]);
		return r;
	}

	static System.Numerics.Vector4[] MakeSNV4 (float[] e, int shift)
	{
		var r = new System.Numerics.Vector4[ROWS];
		for (int i = 0; i < ROWS; i++)
			r[i] = new System.Numerics.Vector4 (e[(i + shift) % e.Length], e[(i + shift + 1) % e.Length],
			                                     e[(i + shift + 2) % e.Length], e[(i + shift + 3) % e.Length]);
		return r;
	}

	static Vector<T> MakeVecT<T> (T[] e, int shift) where T : struct
	{
		int count = Vector<T>.Count;
		var buf = new T[count];
		for (int j = 0; j < count; j++)
			buf[j] = e[(shift + j) % e.Length];
		return new Vector<T> (buf);
	}

	static Vector<T>[] MakeVecTRows<T> (T[] e, int shiftStart) where T : struct
	{
		var r = new Vector<T>[ROWS];
		for (int i = 0; i < ROWS; i++)
			r[i] = MakeVecT<T> (e, shiftStart + i);
		return r;
	}

	static Vector4f[] V4fA = MakeV4f (FE, 0), V4fB = MakeV4f (FE, 5);
	static Vector4i[] V4iA = MakeV4i (IE, 0), V4iB = MakeV4i (IE, 2);
	static Vector4ui[] V4uiA = MakeV4ui (UIE, 0), V4uiB = MakeV4ui (UIE, 2);
	static Vector2d[] V2dA = MakeV2d (DE, 0), V2dB = MakeV2d (DE, 5);
	static Vector2l[] V2lA = MakeV2l (LE, 0), V2lB = MakeV2l (LE, 2);
	static Vector2ul[] V2ulA = MakeV2ul (ULE, 0), V2ulB = MakeV2ul (ULE, 2);
	static Vector8s[] V8sA = MakeV8s (SE, 0), V8sB = MakeV8s (SE, 3);
	static Vector8us[] V8usA = MakeV8us (USE, 0), V8usB = MakeV8us (USE, 3);
	static Vector16b[] V16bA = MakeV16b (BE, 0), V16bB = MakeV16b (BE, 3);
	static Vector16sb[] V16sbA = MakeV16sb (SBE, 0), V16sbB = MakeV16sb (SBE, 3);
	static System.Numerics.Vector4[] SNV4A = MakeSNV4 (FE, 0), SNV4B = MakeSNV4 (FE, 5);

	static Vector<float>[] VFA = MakeVecTRows<float> (FE, 0), VFB = MakeVecTRows<float> (FE, 5);
	static Vector<double>[] VDA = MakeVecTRows<double> (DE, 0), VDB = MakeVecTRows<double> (DE, 5);
	static Vector<int>[] VIA = MakeVecTRows<int> (IE, 0), VIB = MakeVecTRows<int> (IE, 2);
	static Vector<uint>[] VUIA = MakeVecTRows<uint> (UIE, 0), VUIB = MakeVecTRows<uint> (UIE, 2);
	static Vector<long>[] VLA = MakeVecTRows<long> (LE, 0), VLB = MakeVecTRows<long> (LE, 2);
	static Vector<ulong>[] VULA = MakeVecTRows<ulong> (ULE, 0), VULB = MakeVecTRows<ulong> (ULE, 2);
	static Vector<short>[] VSA = MakeVecTRows<short> (SE, 0), VSB = MakeVecTRows<short> (SE, 3);
	static Vector<ushort>[] VUSA = MakeVecTRows<ushort> (USE, 0), VUSB = MakeVecTRows<ushort> (USE, 3);
	static Vector<byte>[] VBA = MakeVecTRows<byte> (BE, 0), VBB = MakeVecTRows<byte> (BE, 3);
	static Vector<sbyte>[] VSBA = MakeVecTRows<sbyte> (SBE, 0), VSBB = MakeVecTRows<sbyte> (SBE, 3);

	static Vector<int>[] VIAabs = MakeVecTRows<int> (IEabs, 0);
	static Vector<long>[] VLAabs = MakeVecTRows<long> (LEabs, 0);
	static Vector<short>[] VSAabs = MakeVecTRows<short> (SEabs, 0);
	static Vector<sbyte>[] VSBAabs = MakeVecTRows<sbyte> (SBEabs, 0);

	static float FA (int row) { return FE[row % FE.Length]; }

	static int Shift (int row) { return ShAmt[row % ShAmt.Length]; }

	// ==================== Mono.Simd: Vector4f ====================

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Add (int r) { return Bytes (V4fA[r] + V4fB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Sub (int r) { return Bytes (V4fA[r] - V4fB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Mul (int r) { return Bytes (V4fA[r] * V4fB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Div (int r) { return Bytes (V4fA[r] / V4fB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_And (int r) { return Bytes (V4fA[r] & V4fB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Or (int r) { return Bytes (V4fA[r] | V4fB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Xor (int r) { return Bytes (V4fA[r] ^ V4fB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Sqrt (int r) { return Bytes (VectorOperations.Sqrt (V4fA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_InvSqrt (int r) { return Bytes (VectorOperations.InvSqrt (V4fA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Reciprocal (int r) { return Bytes (VectorOperations.Reciprocal (V4fA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Min (int r) { return Bytes (VectorOperations.Min (V4fA[r], V4fB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Max (int r) { return Bytes (VectorOperations.Max (V4fA[r], V4fB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_CmpLT (int r) { return Bytes (VectorOperations.CompareLessThan (V4fA[r], V4fB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_CmpLE (int r) { return Bytes (VectorOperations.CompareLessEqual (V4fA[r], V4fB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_CmpNE (int r) { return Bytes (VectorOperations.CompareNotEqual (V4fA[r], V4fB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_CmpUnord (int r) { return Bytes (VectorOperations.CompareUnordered (V4fA[r], V4fB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_CmpOrd (int r) { return Bytes (VectorOperations.CompareOrdered (V4fA[r], V4fB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_HAdd (int r) { return Bytes (VectorOperations.HorizontalAdd (V4fA[r], V4fB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_HSub (int r) { return Bytes (VectorOperations.HorizontalSub (V4fA[r], V4fB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_AddSub (int r) { return Bytes (VectorOperations.AddSub (V4fA[r], V4fB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_DupLo (int r) { return Bytes (VectorOperations.DuplicateLow (V4fA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_DupHi (int r) { return Bytes (VectorOperations.DuplicateHigh (V4fA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_IntHi (int r) { return Bytes (VectorOperations.InterleaveHigh (V4fA[r], V4fB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_IntLo (int r) { return Bytes (VectorOperations.InterleaveLow (V4fA[r], V4fB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Shuf2 (int r) { return Bytes (VectorOperations.Shuffle (V4fA[r], V4fB[r], ShuffleSel.Swap)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Shuf1 (int r) { return Bytes (VectorOperations.Shuffle (V4fA[r], ShuffleSel.ExpandX)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_ToV4i (int r) { return Bytes ((Vector4i) V4fA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_ToV2d (int r) { return Bytes ((Vector2d) V4fA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_ConvInt (int r) { return Bytes (VectorOperations.ConvertToInt (V4fA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_ConvIntTr (int r) { return Bytes (VectorOperations.ConvertToIntTruncated (V4fA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_ConvDbl (int r) { return Bytes (VectorOperations.ConvertToDouble (V4fA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Ctor4 (int r) { return Bytes (new Vector4f (FE[r % FE.Length], FE[(r + 1) % FE.Length], FE[(r + 2) % FE.Length], FE[(r + 3) % FE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_CtorSplat (int r) { return Bytes (new Vector4f (FE[r % FE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4f_Index (int r) { return BitConverter.GetBytes (V4fA[r][r % 4]); }

	static void CheckVector4f ()
	{
		string f = "Mono.Simd";
		Type t = typeof (Vector4f);
		Type[] tt = { t, t };

		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f +", "K_V4f_Add", K_V4f_Add, t.GetMethod ("op_Addition", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f -", "K_V4f_Sub", K_V4f_Sub, t.GetMethod ("op_Subtraction", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f *", "K_V4f_Mul", K_V4f_Mul, t.GetMethod ("op_Multiply", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f /", "K_V4f_Div", K_V4f_Div, t.GetMethod ("op_Division", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f &", "K_V4f_And", K_V4f_And, t.GetMethod ("op_BitwiseAnd", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f |", "K_V4f_Or", K_V4f_Or, t.GetMethod ("op_BitwiseOr", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f ^", "K_V4f_Xor", K_V4f_Xor, t.GetMethod ("op_ExclusiveOr", tt), V4fA, V4fB, Bytes, 4);

		Type vo = typeof (VectorOperations);
		RunUnary<Vector4f, Vector4f> (f, "Vector4f.Sqrt", "K_V4f_Sqrt", K_V4f_Sqrt, vo.GetMethod ("Sqrt", new[] { t }), V4fA, Bytes, 4);
		RunUnary<Vector4f, Vector4f> (f, "Vector4f.InvSqrt", "K_V4f_InvSqrt", K_V4f_InvSqrt, vo.GetMethod ("InvSqrt", new[] { t }), V4fA, Bytes, 4);
		RunUnary<Vector4f, Vector4f> (f, "Vector4f.Reciprocal", "K_V4f_Reciprocal", K_V4f_Reciprocal, vo.GetMethod ("Reciprocal", new[] { t }), V4fA, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f.Min", "K_V4f_Min", K_V4f_Min, vo.GetMethod ("Min", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f.Max", "K_V4f_Max", K_V4f_Max, vo.GetMethod ("Max", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f.CompareLessThan", "K_V4f_CmpLT", K_V4f_CmpLT, vo.GetMethod ("CompareLessThan", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f.CompareLessEqual", "K_V4f_CmpLE", K_V4f_CmpLE, vo.GetMethod ("CompareLessEqual", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f.CompareNotEqual", "K_V4f_CmpNE", K_V4f_CmpNE, vo.GetMethod ("CompareNotEqual", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f.CompareUnordered", "K_V4f_CmpUnord", K_V4f_CmpUnord, vo.GetMethod ("CompareUnordered", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f.CompareOrdered", "K_V4f_CmpOrd", K_V4f_CmpOrd, vo.GetMethod ("CompareOrdered", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f.HorizontalAdd", "K_V4f_HAdd", K_V4f_HAdd, vo.GetMethod ("HorizontalAdd", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f.HorizontalSub", "K_V4f_HSub", K_V4f_HSub, vo.GetMethod ("HorizontalSub", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f.AddSub", "K_V4f_AddSub", K_V4f_AddSub, vo.GetMethod ("AddSub", tt), V4fA, V4fB, Bytes, 4);
		RunUnary<Vector4f, Vector4f> (f, "Vector4f.DuplicateLow", "K_V4f_DupLo", K_V4f_DupLo, vo.GetMethod ("DuplicateLow", new[] { t }), V4fA, Bytes, 4);
		RunUnary<Vector4f, Vector4f> (f, "Vector4f.DuplicateHigh", "K_V4f_DupHi", K_V4f_DupHi, vo.GetMethod ("DuplicateHigh", new[] { t }), V4fA, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f.InterleaveHigh", "K_V4f_IntHi", K_V4f_IntHi, vo.GetMethod ("InterleaveHigh", tt), V4fA, V4fB, Bytes, 4);
		RunBinary<Vector4f, Vector4f, Vector4f> (f, "Vector4f.InterleaveLow", "K_V4f_IntLo", K_V4f_IntLo, vo.GetMethod ("InterleaveLow", tt), V4fA, V4fB, Bytes, 4);
		RunTernary<Vector4f, Vector4f, ShuffleSel, Vector4f> (f, "Vector4f.Shuffle(2)", "K_V4f_Shuf2", K_V4f_Shuf2,
			vo.GetMethod ("Shuffle", new[] { t, t, typeof (ShuffleSel) }), V4fA, V4fB, ShuffleSel.Swap, Bytes, 4);
		RunBinary<Vector4f, ShuffleSel, Vector4f> (f, "Vector4f.Shuffle(1)", "K_V4f_Shuf1", K_V4f_Shuf1,
			vo.GetMethod ("Shuffle", new[] { t, typeof (ShuffleSel) }), V4fA, new[] { ShuffleSel.ExpandX }, Bytes, 4);
		RunUnary<Vector4f, Vector4i> (f, "Vector4f->Vector4i", "K_V4f_ToV4i", K_V4f_ToV4i, Explicit (t, typeof (Vector4i)), V4fA, Bytes, 4);
		RunUnary<Vector4f, Vector2d> (f, "Vector4f->Vector2d", "K_V4f_ToV2d", K_V4f_ToV2d, Explicit (t, typeof (Vector2d)), V4fA, Bytes, 8);
		RunUnary<Vector4f, Vector4i> (f, "Vector4f.ConvertToInt", "K_V4f_ConvInt", K_V4f_ConvInt, vo.GetMethod ("ConvertToInt", new[] { t }), V4fA, Bytes, 4);
		RunUnary<Vector4f, Vector4i> (f, "Vector4f.ConvertToIntTruncated", "K_V4f_ConvIntTr", K_V4f_ConvIntTr, vo.GetMethod ("ConvertToIntTruncated", new[] { t }), V4fA, Bytes, 4);
		RunUnary<Vector4f, Vector2d> (f, "Vector4f.ConvertToDouble", "K_V4f_ConvDbl", K_V4f_ConvDbl, vo.GetMethod ("ConvertToDouble", new[] { t }), V4fA, Bytes, 8);

		RunKernelOnly (f, "Vector4f ctor(4)", "K_V4f_Ctor4", K_V4f_Ctor4, 4);
		RunKernelOnly (f, "Vector4f ctor(splat)", "K_V4f_CtorSplat", K_V4f_CtorSplat, 4);
		RunKernelOnly (f, "Vector4f indexer get", "K_V4f_Index", K_V4f_Index, 4);
	}

	// ==================== Mono.Simd: Vector4i ====================

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_Add (int r) { return Bytes (V4iA[r] + V4iB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_Sub (int r) { return Bytes (V4iA[r] - V4iB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_Mul (int r) { return Bytes (V4iA[r] * V4iB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_Shl (int r) { return Bytes (V4iA[r] << Shift (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_Shr (int r) { return Bytes (V4iA[r] >> Shift (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_And (int r) { return Bytes (V4iA[r] & V4iB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_Or (int r) { return Bytes (V4iA[r] | V4iB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_Xor (int r) { return Bytes (V4iA[r] ^ V4iB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_Min (int r) { return Bytes (VectorOperations.Min (V4iA[r], V4iB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_Max (int r) { return Bytes (VectorOperations.Max (V4iA[r], V4iB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_CmpEq (int r) { return Bytes (VectorOperations.CompareEqual (V4iA[r], V4iB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_CmpGt (int r) { return Bytes (VectorOperations.CompareGreaterThan (V4iA[r], V4iB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_UnpLo (int r) { return Bytes (VectorOperations.UnpackLow (V4iA[r], V4iB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_UnpHi (int r) { return Bytes (VectorOperations.UnpackHigh (V4iA[r], V4iB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_LRShift (int r) { return Bytes (VectorOperations.LogicalRightShift (V4iA[r], Shift (r))); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_ToV4f (int r) { return Bytes ((Vector4f) V4iA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_ToV2l (int r) { return Bytes ((Vector2l) V4iA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_ConvFloat (int r) { return Bytes (VectorOperations.ConvertToFloat (V4iA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_ConvDbl (int r) { return Bytes (VectorOperations.ConvertToDouble (V4iA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_PackS (int r) { return Bytes (VectorOperations.PackWithSignedSaturation (V4iA[r], V4iB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_PackU (int r) { return Bytes (VectorOperations.PackWithUnsignedSaturation (V4iA[r], V4iB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_Ctor4 (int r) { return Bytes (new Vector4i (IE[r % IE.Length], IE[(r + 1) % IE.Length], IE[(r + 2) % IE.Length], IE[(r + 3) % IE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_CtorSplat (int r) { return Bytes (new Vector4i (IE[r % IE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4i_Index (int r) { return BitConverter.GetBytes (V4iA[r][r % 4]); }

	static void CheckVector4i ()
	{
		string f = "Mono.Simd";
		Type t = typeof (Vector4i);
		Type[] tt = { t, t };
		Type[] ti = { t, typeof (int) };
		Type vo = typeof (VectorOperations);

		RunBinary<Vector4i, Vector4i, Vector4i> (f, "Vector4i +", "K_V4i_Add", K_V4i_Add, t.GetMethod ("op_Addition", tt), V4iA, V4iB, Bytes, 4);
		RunBinary<Vector4i, Vector4i, Vector4i> (f, "Vector4i -", "K_V4i_Sub", K_V4i_Sub, t.GetMethod ("op_Subtraction", tt), V4iA, V4iB, Bytes, 4);
		RunBinary<Vector4i, Vector4i, Vector4i> (f, "Vector4i *", "K_V4i_Mul", K_V4i_Mul, t.GetMethod ("op_Multiply", tt), V4iA, V4iB, Bytes, 4);
		RunBinary<Vector4i, int, Vector4i> (f, "Vector4i <<", "K_V4i_Shl", K_V4i_Shl, t.GetMethod ("op_LeftShift", ti), V4iA, ShAmt, Bytes, 4);
		RunBinary<Vector4i, int, Vector4i> (f, "Vector4i >>", "K_V4i_Shr", K_V4i_Shr, t.GetMethod ("op_RightShift", ti), V4iA, ShAmt, Bytes, 4);
		RunBinary<Vector4i, Vector4i, Vector4i> (f, "Vector4i &", "K_V4i_And", K_V4i_And, t.GetMethod ("op_BitwiseAnd", tt), V4iA, V4iB, Bytes, 4);
		RunBinary<Vector4i, Vector4i, Vector4i> (f, "Vector4i |", "K_V4i_Or", K_V4i_Or, t.GetMethod ("op_BitwiseOr", tt), V4iA, V4iB, Bytes, 4);
		RunBinary<Vector4i, Vector4i, Vector4i> (f, "Vector4i ^", "K_V4i_Xor", K_V4i_Xor, t.GetMethod ("op_ExclusiveOr", tt), V4iA, V4iB, Bytes, 4);
		RunBinary<Vector4i, Vector4i, Vector4i> (f, "Vector4i.Min", "K_V4i_Min", K_V4i_Min, vo.GetMethod ("Min", tt), V4iA, V4iB, Bytes, 4);
		RunBinary<Vector4i, Vector4i, Vector4i> (f, "Vector4i.Max", "K_V4i_Max", K_V4i_Max, vo.GetMethod ("Max", tt), V4iA, V4iB, Bytes, 4);
		RunBinary<Vector4i, Vector4i, Vector4i> (f, "Vector4i.CompareEqual", "K_V4i_CmpEq", K_V4i_CmpEq, vo.GetMethod ("CompareEqual", tt), V4iA, V4iB, Bytes, 4);
		RunBinary<Vector4i, Vector4i, Vector4i> (f, "Vector4i.CompareGreaterThan", "K_V4i_CmpGt", K_V4i_CmpGt, vo.GetMethod ("CompareGreaterThan", tt), V4iA, V4iB, Bytes, 4);
		RunBinary<Vector4i, Vector4i, Vector4i> (f, "Vector4i.UnpackLow", "K_V4i_UnpLo", K_V4i_UnpLo, vo.GetMethod ("UnpackLow", tt), V4iA, V4iB, Bytes, 4);
		RunBinary<Vector4i, Vector4i, Vector4i> (f, "Vector4i.UnpackHigh", "K_V4i_UnpHi", K_V4i_UnpHi, vo.GetMethod ("UnpackHigh", tt), V4iA, V4iB, Bytes, 4);
		RunBinary<Vector4i, int, Vector4i> (f, "Vector4i.LogicalRightShift", "K_V4i_LRShift", K_V4i_LRShift, vo.GetMethod ("LogicalRightShift", ti), V4iA, ShAmt, Bytes, 4);
		RunUnary<Vector4i, Vector4f> (f, "Vector4i->Vector4f", "K_V4i_ToV4f", K_V4i_ToV4f, Explicit (t, typeof (Vector4f)), V4iA, Bytes, 4);
		RunUnary<Vector4i, Vector2l> (f, "Vector4i->Vector2l", "K_V4i_ToV2l", K_V4i_ToV2l, Explicit (t, typeof (Vector2l)), V4iA, Bytes, 8);
		RunUnary<Vector4i, Vector4f> (f, "Vector4i.ConvertToFloat", "K_V4i_ConvFloat", K_V4i_ConvFloat, vo.GetMethod ("ConvertToFloat", new[] { t }), V4iA, Bytes, 4);
		RunUnary<Vector4i, Vector2d> (f, "Vector4i.ConvertToDouble", "K_V4i_ConvDbl", K_V4i_ConvDbl, vo.GetMethod ("ConvertToDouble", new[] { t }), V4iA, Bytes, 8);
		RunBinary<Vector4i, Vector4i, Vector8s> (f, "Vector4i.PackWithSignedSaturation", "K_V4i_PackS", K_V4i_PackS, vo.GetMethod ("PackWithSignedSaturation", tt), V4iA, V4iB, Bytes, 2);
		RunBinary<Vector4i, Vector4i, Vector8us> (f, "Vector4i.PackWithUnsignedSaturation", "K_V4i_PackU", K_V4i_PackU, vo.GetMethod ("PackWithUnsignedSaturation", tt), V4iA, V4iB, Bytes, 2);

		RunKernelOnly (f, "Vector4i ctor(4)", "K_V4i_Ctor4", K_V4i_Ctor4, 4);
		RunKernelOnly (f, "Vector4i ctor(splat)", "K_V4i_CtorSplat", K_V4i_CtorSplat, 4);
		RunKernelOnly (f, "Vector4i indexer get", "K_V4i_Index", K_V4i_Index, 4);
	}

	// ==================== Mono.Simd: Vector4ui ====================

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_Add (int r) { return Bytes (V4uiA[r] + V4uiB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_Sub (int r) { return Bytes (V4uiA[r] - V4uiB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_Mul (int r) { return Bytes (V4uiA[r] * V4uiB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_Shl (int r) { return Bytes (V4uiA[r] << Shift (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_Shr (int r) { return Bytes (V4uiA[r] >> Shift (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_And (int r) { return Bytes (V4uiA[r] & V4uiB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_Or (int r) { return Bytes (V4uiA[r] | V4uiB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_Xor (int r) { return Bytes (V4uiA[r] ^ V4uiB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_Min (int r) { return Bytes (VectorOperations.Min (V4uiA[r], V4uiB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_Max (int r) { return Bytes (VectorOperations.Max (V4uiA[r], V4uiB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_CmpEq (int r) { return Bytes (VectorOperations.CompareEqual (V4uiA[r], V4uiB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_ARShift (int r) { return Bytes (VectorOperations.ArithmeticRightShift (V4uiA[r], Shift (r))); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_ToV4f (int r) { return Bytes ((Vector4f) V4uiA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_ToV4i (int r) { return Bytes ((Vector4i) V4uiA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_Ctor4 (int r) { return Bytes (new Vector4ui (UIE[r % UIE.Length], UIE[(r + 1) % UIE.Length], UIE[(r + 2) % UIE.Length], UIE[(r + 3) % UIE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_CtorSplat (int r) { return Bytes (new Vector4ui (UIE[r % UIE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V4ui_Index (int r) { return BitConverter.GetBytes (V4uiA[r][r % 4]); }

	static void CheckVector4ui ()
	{
		string f = "Mono.Simd";
		Type t = typeof (Vector4ui);
		Type[] tt = { t, t };
		Type[] ti = { t, typeof (int) };
		Type vo = typeof (VectorOperations);

		RunBinary<Vector4ui, Vector4ui, Vector4ui> (f, "Vector4ui +", "K_V4ui_Add", K_V4ui_Add, t.GetMethod ("op_Addition", tt), V4uiA, V4uiB, Bytes, 4);
		RunBinary<Vector4ui, Vector4ui, Vector4ui> (f, "Vector4ui -", "K_V4ui_Sub", K_V4ui_Sub, t.GetMethod ("op_Subtraction", tt), V4uiA, V4uiB, Bytes, 4);
		RunBinary<Vector4ui, Vector4ui, Vector4ui> (f, "Vector4ui *", "K_V4ui_Mul", K_V4ui_Mul, t.GetMethod ("op_Multiply", tt), V4uiA, V4uiB, Bytes, 4);
		RunBinary<Vector4ui, int, Vector4ui> (f, "Vector4ui <<", "K_V4ui_Shl", K_V4ui_Shl, t.GetMethod ("op_LeftShift", ti), V4uiA, ShAmt, Bytes, 4);
		RunBinary<Vector4ui, int, Vector4ui> (f, "Vector4ui >>", "K_V4ui_Shr", K_V4ui_Shr, t.GetMethod ("op_RightShift", ti), V4uiA, ShAmt, Bytes, 4);
		RunBinary<Vector4ui, Vector4ui, Vector4ui> (f, "Vector4ui &", "K_V4ui_And", K_V4ui_And, t.GetMethod ("op_BitwiseAnd", tt), V4uiA, V4uiB, Bytes, 4);
		RunBinary<Vector4ui, Vector4ui, Vector4ui> (f, "Vector4ui |", "K_V4ui_Or", K_V4ui_Or, t.GetMethod ("op_BitwiseOr", tt), V4uiA, V4uiB, Bytes, 4);
		RunBinary<Vector4ui, Vector4ui, Vector4ui> (f, "Vector4ui ^", "K_V4ui_Xor", K_V4ui_Xor, t.GetMethod ("op_ExclusiveOr", tt), V4uiA, V4uiB, Bytes, 4);
		RunBinary<Vector4ui, Vector4ui, Vector4ui> (f, "Vector4ui.Min", "K_V4ui_Min", K_V4ui_Min, vo.GetMethod ("Min", tt), V4uiA, V4uiB, Bytes, 4);
		RunBinary<Vector4ui, Vector4ui, Vector4ui> (f, "Vector4ui.Max", "K_V4ui_Max", K_V4ui_Max, vo.GetMethod ("Max", tt), V4uiA, V4uiB, Bytes, 4);
		RunBinary<Vector4ui, Vector4ui, Vector4ui> (f, "Vector4ui.CompareEqual", "K_V4ui_CmpEq", K_V4ui_CmpEq, vo.GetMethod ("CompareEqual", tt), V4uiA, V4uiB, Bytes, 4);
		RunBinary<Vector4ui, int, Vector4ui> (f, "Vector4ui.ArithmeticRightShift", "K_V4ui_ARShift", K_V4ui_ARShift, vo.GetMethod ("ArithmeticRightShift", ti), V4uiA, ShAmt, Bytes, 4);
		RunUnary<Vector4ui, Vector4f> (f, "Vector4ui->Vector4f", "K_V4ui_ToV4f", K_V4ui_ToV4f, Explicit (t, typeof (Vector4f)), V4uiA, Bytes, 4);
		RunUnary<Vector4ui, Vector4i> (f, "Vector4ui->Vector4i", "K_V4ui_ToV4i", K_V4ui_ToV4i, Explicit (t, typeof (Vector4i)), V4uiA, Bytes, 4);

		RunKernelOnly (f, "Vector4ui ctor(4)", "K_V4ui_Ctor4", K_V4ui_Ctor4, 4);
		RunKernelOnly (f, "Vector4ui ctor(splat)", "K_V4ui_CtorSplat", K_V4ui_CtorSplat, 4);
		RunKernelOnly (f, "Vector4ui indexer get", "K_V4ui_Index", K_V4ui_Index, 4);
	}

	// ==================== Mono.Simd: Vector2d ====================

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_Add (int r) { return Bytes (V2dA[r] + V2dB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_Sub (int r) { return Bytes (V2dA[r] - V2dB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_Mul (int r) { return Bytes (V2dA[r] * V2dB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_Div (int r) { return Bytes (V2dA[r] / V2dB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_And (int r) { return Bytes (V2dA[r] & V2dB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_Or (int r) { return Bytes (V2dA[r] | V2dB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_Xor (int r) { return Bytes (V2dA[r] ^ V2dB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_Sqrt (int r) { return Bytes (VectorOperations.Sqrt (V2dA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_Min (int r) { return Bytes (VectorOperations.Min (V2dA[r], V2dB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_Max (int r) { return Bytes (VectorOperations.Max (V2dA[r], V2dB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_CmpLT (int r) { return Bytes (VectorOperations.CompareLessThan (V2dA[r], V2dB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_CmpNE (int r) { return Bytes (VectorOperations.CompareNotEqual (V2dA[r], V2dB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_CmpUnord (int r) { return Bytes (VectorOperations.CompareUnordered (V2dA[r], V2dB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_CmpOrd (int r) { return Bytes (VectorOperations.CompareOrdered (V2dA[r], V2dB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_HAdd (int r) { return Bytes (VectorOperations.HorizontalAdd (V2dA[r], V2dB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_HSub (int r) { return Bytes (VectorOperations.HorizontalSub (V2dA[r], V2dB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_AddSub (int r) { return Bytes (VectorOperations.AddSub (V2dA[r], V2dB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_Dup (int r) { return Bytes (VectorOperations.Duplicate (V2dA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_IntHi (int r) { return Bytes (VectorOperations.InterleaveHigh (V2dA[r], V2dB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_IntLo (int r) { return Bytes (VectorOperations.InterleaveLow (V2dA[r], V2dB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_ToV4f (int r) { return Bytes ((Vector4f) V2dA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_ToV2l (int r) { return Bytes ((Vector2l) V2dA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_ConvInt (int r) { return Bytes (VectorOperations.ConvertToInt (V2dA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_ConvIntTr (int r) { return Bytes (VectorOperations.ConvertToIntTruncated (V2dA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_ConvFloat (int r) { return Bytes (VectorOperations.ConvertToFloat (V2dA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_Ctor2 (int r) { return Bytes (new Vector2d (DE[r % DE.Length], DE[(r + 1) % DE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_CtorSplat (int r) { return Bytes (new Vector2d (DE[r % DE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2d_Index (int r) { return BitConverter.GetBytes (V2dA[r][r % 2]); }

	static void CheckVector2d ()
	{
		string f = "Mono.Simd";
		Type t = typeof (Vector2d);
		Type[] tt = { t, t };
		Type vo = typeof (VectorOperations);

		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d +", "K_V2d_Add", K_V2d_Add, t.GetMethod ("op_Addition", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d -", "K_V2d_Sub", K_V2d_Sub, t.GetMethod ("op_Subtraction", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d *", "K_V2d_Mul", K_V2d_Mul, t.GetMethod ("op_Multiply", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d /", "K_V2d_Div", K_V2d_Div, t.GetMethod ("op_Division", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d &", "K_V2d_And", K_V2d_And, t.GetMethod ("op_BitwiseAnd", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d |", "K_V2d_Or", K_V2d_Or, t.GetMethod ("op_BitwiseOr", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d ^", "K_V2d_Xor", K_V2d_Xor, t.GetMethod ("op_ExclusiveOr", tt), V2dA, V2dB, Bytes, 8);
		RunUnary<Vector2d, Vector2d> (f, "Vector2d.Sqrt", "K_V2d_Sqrt", K_V2d_Sqrt, vo.GetMethod ("Sqrt", new[] { t }), V2dA, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d.Min", "K_V2d_Min", K_V2d_Min, vo.GetMethod ("Min", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d.Max", "K_V2d_Max", K_V2d_Max, vo.GetMethod ("Max", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d.CompareLessThan", "K_V2d_CmpLT", K_V2d_CmpLT, vo.GetMethod ("CompareLessThan", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d.CompareNotEqual", "K_V2d_CmpNE", K_V2d_CmpNE, vo.GetMethod ("CompareNotEqual", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d.CompareUnordered", "K_V2d_CmpUnord", K_V2d_CmpUnord, vo.GetMethod ("CompareUnordered", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d.CompareOrdered", "K_V2d_CmpOrd", K_V2d_CmpOrd, vo.GetMethod ("CompareOrdered", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d.HorizontalAdd", "K_V2d_HAdd", K_V2d_HAdd, vo.GetMethod ("HorizontalAdd", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d.HorizontalSub", "K_V2d_HSub", K_V2d_HSub, vo.GetMethod ("HorizontalSub", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d.AddSub", "K_V2d_AddSub", K_V2d_AddSub, vo.GetMethod ("AddSub", tt), V2dA, V2dB, Bytes, 8);
		RunUnary<Vector2d, Vector2d> (f, "Vector2d.Duplicate", "K_V2d_Dup", K_V2d_Dup, vo.GetMethod ("Duplicate", new[] { t }), V2dA, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d.InterleaveHigh", "K_V2d_IntHi", K_V2d_IntHi, vo.GetMethod ("InterleaveHigh", tt), V2dA, V2dB, Bytes, 8);
		RunBinary<Vector2d, Vector2d, Vector2d> (f, "Vector2d.InterleaveLow", "K_V2d_IntLo", K_V2d_IntLo, vo.GetMethod ("InterleaveLow", tt), V2dA, V2dB, Bytes, 8);
		RunUnary<Vector2d, Vector4f> (f, "Vector2d->Vector4f", "K_V2d_ToV4f", K_V2d_ToV4f, Explicit (t, typeof (Vector4f)), V2dA, Bytes, 4);
		RunUnary<Vector2d, Vector2l> (f, "Vector2d->Vector2l", "K_V2d_ToV2l", K_V2d_ToV2l, Explicit (t, typeof (Vector2l)), V2dA, Bytes, 8);
		RunUnary<Vector2d, Vector4i> (f, "Vector2d.ConvertToInt", "K_V2d_ConvInt", K_V2d_ConvInt, vo.GetMethod ("ConvertToInt", new[] { t }), V2dA, Bytes, 4);
		RunUnary<Vector2d, Vector4i> (f, "Vector2d.ConvertToIntTruncated", "K_V2d_ConvIntTr", K_V2d_ConvIntTr, vo.GetMethod ("ConvertToIntTruncated", new[] { t }), V2dA, Bytes, 4);
		RunUnary<Vector2d, Vector4f> (f, "Vector2d.ConvertToFloat", "K_V2d_ConvFloat", K_V2d_ConvFloat, vo.GetMethod ("ConvertToFloat", new[] { t }), V2dA, Bytes, 4);

		RunKernelOnly (f, "Vector2d ctor(2)", "K_V2d_Ctor2", K_V2d_Ctor2, 8);
		RunKernelOnly (f, "Vector2d ctor(splat)", "K_V2d_CtorSplat", K_V2d_CtorSplat, 8);
		RunKernelOnly (f, "Vector2d indexer get", "K_V2d_Index", K_V2d_Index, 8);
	}

	// ==================== Mono.Simd: Vector2l / Vector2ul ====================

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_Add (int r) { return Bytes (V2lA[r] + V2lB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_Sub (int r) { return Bytes (V2lA[r] - V2lB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_Shl (int r) { return Bytes (V2lA[r] << Shift (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_And (int r) { return Bytes (V2lA[r] & V2lB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_Or (int r) { return Bytes (V2lA[r] | V2lB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_Xor (int r) { return Bytes (V2lA[r] ^ V2lB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_LRShift (int r) { return Bytes (VectorOperations.LogicalRightShift (V2lA[r], Shift (r))); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_CmpEq (int r) { return Bytes (VectorOperations.CompareEqual (V2lA[r], V2lB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_CmpGt (int r) { return Bytes (VectorOperations.CompareGreaterThan (V2lA[r], V2lB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_UnpLo (int r) { return Bytes (VectorOperations.UnpackLow (V2lA[r], V2lB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_UnpHi (int r) { return Bytes (VectorOperations.UnpackHigh (V2lA[r], V2lB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_ToV2d (int r) { return Bytes ((Vector2d) V2lA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_ToV4i (int r) { return Bytes ((Vector4i) V2lA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_Ctor2 (int r) { return Bytes (new Vector2l (LE[r % LE.Length], LE[(r + 1) % LE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_CtorSplat (int r) { return Bytes (new Vector2l (LE[r % LE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2l_Index (int r) { return BitConverter.GetBytes (V2lA[r][r % 2]); }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_Add (int r) { return Bytes (V2ulA[r] + V2ulB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_Sub (int r) { return Bytes (V2ulA[r] - V2ulB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_Shl (int r) { return Bytes (V2ulA[r] << Shift (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_Shr (int r) { return Bytes (V2ulA[r] >> Shift (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_And (int r) { return Bytes (V2ulA[r] & V2ulB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_Or (int r) { return Bytes (V2ulA[r] | V2ulB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_Xor (int r) { return Bytes (V2ulA[r] ^ V2ulB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_CmpEq (int r) { return Bytes (VectorOperations.CompareEqual (V2ulA[r], V2ulB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_UnpLo (int r) { return Bytes (VectorOperations.UnpackLow (V2ulA[r], V2ulB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_UnpHi (int r) { return Bytes (VectorOperations.UnpackHigh (V2ulA[r], V2ulB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_ToV2d (int r) { return Bytes ((Vector2d) V2ulA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_ToV2l (int r) { return Bytes ((Vector2l) V2ulA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_Ctor2 (int r) { return Bytes (new Vector2ul (ULE[r % ULE.Length], ULE[(r + 1) % ULE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_CtorSplat (int r) { return Bytes (new Vector2ul (ULE[r % ULE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V2ul_Index (int r) { return BitConverter.GetBytes (V2ulA[r][r % 2]); }

	static void CheckVector2l ()
	{
		string f = "Mono.Simd";
		Type t = typeof (Vector2l);
		Type[] tt = { t, t };
		Type[] ti = { t, typeof (int) };
		Type vo = typeof (VectorOperations);

		RunBinary<Vector2l, Vector2l, Vector2l> (f, "Vector2l +", "K_V2l_Add", K_V2l_Add, t.GetMethod ("op_Addition", tt), V2lA, V2lB, Bytes, 8);
		RunBinary<Vector2l, Vector2l, Vector2l> (f, "Vector2l -", "K_V2l_Sub", K_V2l_Sub, t.GetMethod ("op_Subtraction", tt), V2lA, V2lB, Bytes, 8);
		RunBinary<Vector2l, int, Vector2l> (f, "Vector2l <<", "K_V2l_Shl", K_V2l_Shl, t.GetMethod ("op_LeftShift", ti), V2lA, ShAmt, Bytes, 8);
		RunBinary<Vector2l, Vector2l, Vector2l> (f, "Vector2l &", "K_V2l_And", K_V2l_And, t.GetMethod ("op_BitwiseAnd", tt), V2lA, V2lB, Bytes, 8);
		RunBinary<Vector2l, Vector2l, Vector2l> (f, "Vector2l |", "K_V2l_Or", K_V2l_Or, t.GetMethod ("op_BitwiseOr", tt), V2lA, V2lB, Bytes, 8);
		RunBinary<Vector2l, Vector2l, Vector2l> (f, "Vector2l ^", "K_V2l_Xor", K_V2l_Xor, t.GetMethod ("op_ExclusiveOr", tt), V2lA, V2lB, Bytes, 8);
		RunBinary<Vector2l, int, Vector2l> (f, "Vector2l.LogicalRightShift", "K_V2l_LRShift", K_V2l_LRShift, vo.GetMethod ("LogicalRightShift", ti), V2lA, ShAmt, Bytes, 8);
		RunBinary<Vector2l, Vector2l, Vector2l> (f, "Vector2l.CompareEqual", "K_V2l_CmpEq", K_V2l_CmpEq, vo.GetMethod ("CompareEqual", tt), V2lA, V2lB, Bytes, 8);
		RunBinary<Vector2l, Vector2l, Vector2l> (f, "Vector2l.CompareGreaterThan", "K_V2l_CmpGt", K_V2l_CmpGt, vo.GetMethod ("CompareGreaterThan", tt), V2lA, V2lB, Bytes, 8);
		RunBinary<Vector2l, Vector2l, Vector2l> (f, "Vector2l.UnpackLow", "K_V2l_UnpLo", K_V2l_UnpLo, vo.GetMethod ("UnpackLow", tt), V2lA, V2lB, Bytes, 8);
		RunBinary<Vector2l, Vector2l, Vector2l> (f, "Vector2l.UnpackHigh", "K_V2l_UnpHi", K_V2l_UnpHi, vo.GetMethod ("UnpackHigh", tt), V2lA, V2lB, Bytes, 8);
		RunUnary<Vector2l, Vector2d> (f, "Vector2l->Vector2d", "K_V2l_ToV2d", K_V2l_ToV2d, Explicit (t, typeof (Vector2d)), V2lA, Bytes, 8);
		RunUnary<Vector2l, Vector4i> (f, "Vector2l->Vector4i", "K_V2l_ToV4i", K_V2l_ToV4i, Explicit (t, typeof (Vector4i)), V2lA, Bytes, 4);

		RunKernelOnly (f, "Vector2l ctor(2)", "K_V2l_Ctor2", K_V2l_Ctor2, 8);
		RunKernelOnly (f, "Vector2l ctor(splat)", "K_V2l_CtorSplat", K_V2l_CtorSplat, 8);
		RunKernelOnly (f, "Vector2l indexer get", "K_V2l_Index", K_V2l_Index, 8);

		Type tu = typeof (Vector2ul);
		Type[] tut = { tu, tu };
		Type[] tui = { tu, typeof (int) };

		RunBinary<Vector2ul, Vector2ul, Vector2ul> (f, "Vector2ul +", "K_V2ul_Add", K_V2ul_Add, tu.GetMethod ("op_Addition", tut), V2ulA, V2ulB, Bytes, 8);
		RunBinary<Vector2ul, Vector2ul, Vector2ul> (f, "Vector2ul -", "K_V2ul_Sub", K_V2ul_Sub, tu.GetMethod ("op_Subtraction", tut), V2ulA, V2ulB, Bytes, 8);
		RunBinary<Vector2ul, int, Vector2ul> (f, "Vector2ul <<", "K_V2ul_Shl", K_V2ul_Shl, tu.GetMethod ("op_LeftShift", tui), V2ulA, ShAmt, Bytes, 8);
		RunBinary<Vector2ul, int, Vector2ul> (f, "Vector2ul >>", "K_V2ul_Shr", K_V2ul_Shr, tu.GetMethod ("op_RightShift", tui), V2ulA, ShAmt, Bytes, 8);
		RunBinary<Vector2ul, Vector2ul, Vector2ul> (f, "Vector2ul &", "K_V2ul_And", K_V2ul_And, tu.GetMethod ("op_BitwiseAnd", tut), V2ulA, V2ulB, Bytes, 8);
		RunBinary<Vector2ul, Vector2ul, Vector2ul> (f, "Vector2ul |", "K_V2ul_Or", K_V2ul_Or, tu.GetMethod ("op_BitwiseOr", tut), V2ulA, V2ulB, Bytes, 8);
		RunBinary<Vector2ul, Vector2ul, Vector2ul> (f, "Vector2ul ^", "K_V2ul_Xor", K_V2ul_Xor, tu.GetMethod ("op_ExclusiveOr", tut), V2ulA, V2ulB, Bytes, 8);
		RunBinary<Vector2ul, Vector2ul, Vector2ul> (f, "Vector2ul.CompareEqual", "K_V2ul_CmpEq", K_V2ul_CmpEq, vo.GetMethod ("CompareEqual", tut), V2ulA, V2ulB, Bytes, 8);
		RunBinary<Vector2ul, Vector2ul, Vector2ul> (f, "Vector2ul.UnpackLow", "K_V2ul_UnpLo", K_V2ul_UnpLo, vo.GetMethod ("UnpackLow", tut), V2ulA, V2ulB, Bytes, 8);
		RunBinary<Vector2ul, Vector2ul, Vector2ul> (f, "Vector2ul.UnpackHigh", "K_V2ul_UnpHi", K_V2ul_UnpHi, vo.GetMethod ("UnpackHigh", tut), V2ulA, V2ulB, Bytes, 8);
		RunUnary<Vector2ul, Vector2d> (f, "Vector2ul->Vector2d", "K_V2ul_ToV2d", K_V2ul_ToV2d, Explicit (tu, typeof (Vector2d)), V2ulA, Bytes, 8);
		RunUnary<Vector2ul, Vector2l> (f, "Vector2ul->Vector2l", "K_V2ul_ToV2l", K_V2ul_ToV2l, Explicit (tu, typeof (Vector2l)), V2ulA, Bytes, 8);

		RunKernelOnly (f, "Vector2ul ctor(2)", "K_V2ul_Ctor2", K_V2ul_Ctor2, 8);
		RunKernelOnly (f, "Vector2ul ctor(splat)", "K_V2ul_CtorSplat", K_V2ul_CtorSplat, 8);
		RunKernelOnly (f, "Vector2ul indexer get", "K_V2ul_Index", K_V2ul_Index, 8);
	}

	// ==================== Mono.Simd: Vector8s / Vector8us ====================

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_Add (int r) { return Bytes (V8sA[r] + V8sB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_Sub (int r) { return Bytes (V8sA[r] - V8sB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_Mul (int r) { return Bytes (V8sA[r] * V8sB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_Shr (int r) { return Bytes (V8sA[r] >> Shift (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_Shl (int r) { return Bytes (V8sA[r] << Shift (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_And (int r) { return Bytes (V8sA[r] & V8sB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_Or (int r) { return Bytes (V8sA[r] | V8sB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_Xor (int r) { return Bytes (V8sA[r] ^ V8sB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_Min (int r) { return Bytes (VectorOperations.Min (V8sA[r], V8sB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_Max (int r) { return Bytes (VectorOperations.Max (V8sA[r], V8sB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_CmpEq (int r) { return Bytes (VectorOperations.CompareEqual (V8sA[r], V8sB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_CmpGt (int r) { return Bytes (VectorOperations.CompareGreaterThan (V8sA[r], V8sB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_AddSat (int r) { return Bytes (VectorOperations.AddWithSaturation (V8sA[r], V8sB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_SubSat (int r) { return Bytes (VectorOperations.SubtractWithSaturation (V8sA[r], V8sB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_MulHi (int r) { return Bytes (VectorOperations.MultiplyStoreHigh (V8sA[r], V8sB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_UnpLo (int r) { return Bytes (VectorOperations.UnpackLow (V8sA[r], V8sB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_UnpHi (int r) { return Bytes (VectorOperations.UnpackHigh (V8sA[r], V8sB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_ShufHi (int r) { return Bytes (VectorOperations.ShuffleHigh (V8sA[r], ShuffleSel.Swap)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_ShufLo (int r) { return Bytes (VectorOperations.ShuffleLow (V8sA[r], ShuffleSel.Swap)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_ToV4i (int r) { return Bytes ((Vector4i) V8sA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_ToV8us (int r) { return Bytes ((Vector8us) V8sA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_PackS (int r) { return Bytes (VectorOperations.PackWithSignedSaturation (V8sA[r], V8sB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_PackU (int r) { return Bytes (VectorOperations.PackWithUnsignedSaturation (V8sA[r], V8sB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)]
	static byte[] K_V8s_Ctor8 (int r)
	{
		return Bytes (new Vector8s (SE[r % SE.Length], SE[(r + 1) % SE.Length], SE[(r + 2) % SE.Length], SE[(r + 3) % SE.Length],
		                             SE[(r + 4) % SE.Length], SE[(r + 5) % SE.Length], SE[(r + 6) % SE.Length], SE[(r + 7) % SE.Length]));
	}
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_CtorSplat (int r) { return Bytes (new Vector8s (SE[r % SE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8s_Index (int r) { return BitConverter.GetBytes (V8sA[r][r % 8]); }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_Add (int r) { return Bytes (V8usA[r] + V8usB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_Sub (int r) { return Bytes (V8usA[r] - V8usB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_Mul (int r) { return Bytes (V8usA[r] * V8usB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_Shr (int r) { return Bytes (V8usA[r] >> Shift (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_Shl (int r) { return Bytes (V8usA[r] << Shift (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_And (int r) { return Bytes (V8usA[r] & V8usB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_Or (int r) { return Bytes (V8usA[r] | V8usB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_Xor (int r) { return Bytes (V8usA[r] ^ V8usB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_Min (int r) { return Bytes (VectorOperations.Min (V8usA[r], V8usB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_Max (int r) { return Bytes (VectorOperations.Max (V8usA[r], V8usB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_Avg (int r) { return Bytes (VectorOperations.Average (V8usA[r], V8usB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_CmpEq (int r) { return Bytes (VectorOperations.CompareEqual (V8usA[r], V8usB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_AddSat (int r) { return Bytes (VectorOperations.AddWithSaturation (V8usA[r], V8usB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_SubSat (int r) { return Bytes (VectorOperations.SubtractWithSaturation (V8usA[r], V8usB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_MulHi (int r) { return Bytes (VectorOperations.MultiplyStoreHigh (V8usA[r], V8usB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_UnpLo (int r) { return Bytes (VectorOperations.UnpackLow (V8usA[r], V8usB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_UnpHi (int r) { return Bytes (VectorOperations.UnpackHigh (V8usA[r], V8usB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_ShufHi (int r) { return Bytes (VectorOperations.ShuffleHigh (V8usA[r], ShuffleSel.Swap)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_ShufLo (int r) { return Bytes (VectorOperations.ShuffleLow (V8usA[r], ShuffleSel.Swap)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_ARShift (int r) { return Bytes (VectorOperations.ArithmeticRightShift (V8usA[r], Shift (r))); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_ToV8s (int r) { return Bytes ((Vector8s) V8usA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_PackU (int r) { return Bytes (VectorOperations.SignedPackWithUnsignedSaturation (V8usA[r], V8usB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)]
	static byte[] K_V8us_Ctor8 (int r)
	{
		return Bytes (new Vector8us (USE[r % USE.Length], USE[(r + 1) % USE.Length], USE[(r + 2) % USE.Length], USE[(r + 3) % USE.Length],
		                              USE[(r + 4) % USE.Length], USE[(r + 5) % USE.Length], USE[(r + 6) % USE.Length], USE[(r + 7) % USE.Length]));
	}
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_CtorSplat (int r) { return Bytes (new Vector8us (USE[r % USE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V8us_Index (int r) { return BitConverter.GetBytes (V8usA[r][r % 8]); }

	static void CheckVector8s ()
	{
		string f = "Mono.Simd";
		Type t = typeof (Vector8s);
		Type[] tt = { t, t };
		Type[] ti = { t, typeof (int) };
		Type[] tsel = { t, typeof (ShuffleSel) };
		Type vo = typeof (VectorOperations);

		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s +", "K_V8s_Add", K_V8s_Add, t.GetMethod ("op_Addition", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s -", "K_V8s_Sub", K_V8s_Sub, t.GetMethod ("op_Subtraction", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s *", "K_V8s_Mul", K_V8s_Mul, t.GetMethod ("op_Multiply", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, int, Vector8s> (f, "Vector8s >>", "K_V8s_Shr", K_V8s_Shr, t.GetMethod ("op_RightShift", ti), V8sA, ShAmt, Bytes, 2);
		RunBinary<Vector8s, int, Vector8s> (f, "Vector8s <<", "K_V8s_Shl", K_V8s_Shl, t.GetMethod ("op_LeftShift", ti), V8sA, ShAmt, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s &", "K_V8s_And", K_V8s_And, t.GetMethod ("op_BitwiseAnd", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s |", "K_V8s_Or", K_V8s_Or, t.GetMethod ("op_BitwiseOr", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s ^", "K_V8s_Xor", K_V8s_Xor, t.GetMethod ("op_ExclusiveOr", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s.Min", "K_V8s_Min", K_V8s_Min, vo.GetMethod ("Min", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s.Max", "K_V8s_Max", K_V8s_Max, vo.GetMethod ("Max", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s.CompareEqual", "K_V8s_CmpEq", K_V8s_CmpEq, vo.GetMethod ("CompareEqual", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s.CompareGreaterThan", "K_V8s_CmpGt", K_V8s_CmpGt, vo.GetMethod ("CompareGreaterThan", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s.AddWithSaturation", "K_V8s_AddSat", K_V8s_AddSat, vo.GetMethod ("AddWithSaturation", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s.SubtractWithSaturation", "K_V8s_SubSat", K_V8s_SubSat, vo.GetMethod ("SubtractWithSaturation", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s.MultiplyStoreHigh", "K_V8s_MulHi", K_V8s_MulHi, vo.GetMethod ("MultiplyStoreHigh", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s.UnpackLow", "K_V8s_UnpLo", K_V8s_UnpLo, vo.GetMethod ("UnpackLow", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector8s> (f, "Vector8s.UnpackHigh", "K_V8s_UnpHi", K_V8s_UnpHi, vo.GetMethod ("UnpackHigh", tt), V8sA, V8sB, Bytes, 2);
		RunBinary<Vector8s, ShuffleSel, Vector8s> (f, "Vector8s.ShuffleHigh", "K_V8s_ShufHi", K_V8s_ShufHi, vo.GetMethod ("ShuffleHigh", tsel), V8sA, new[] { ShuffleSel.Swap }, Bytes, 2);
		RunBinary<Vector8s, ShuffleSel, Vector8s> (f, "Vector8s.ShuffleLow", "K_V8s_ShufLo", K_V8s_ShufLo, vo.GetMethod ("ShuffleLow", tsel), V8sA, new[] { ShuffleSel.Swap }, Bytes, 2);
		RunUnary<Vector8s, Vector4i> (f, "Vector8s->Vector4i", "K_V8s_ToV4i", K_V8s_ToV4i, Explicit (t, typeof (Vector4i)), V8sA, Bytes, 4);
		RunUnary<Vector8s, Vector8us> (f, "Vector8s->Vector8us", "K_V8s_ToV8us", K_V8s_ToV8us, Explicit (t, typeof (Vector8us)), V8sA, Bytes, 2);
		RunBinary<Vector8s, Vector8s, Vector16sb> (f, "Vector8s.PackWithSignedSaturation", "K_V8s_PackS", K_V8s_PackS, vo.GetMethod ("PackWithSignedSaturation", tt), V8sA, V8sB, Bytes, 1);
		RunBinary<Vector8s, Vector8s, Vector16b> (f, "Vector8s.PackWithUnsignedSaturation", "K_V8s_PackU", K_V8s_PackU, vo.GetMethod ("PackWithUnsignedSaturation", tt), V8sA, V8sB, Bytes, 1);

		RunKernelOnly (f, "Vector8s ctor(8)", "K_V8s_Ctor8", K_V8s_Ctor8, 2);
		RunKernelOnly (f, "Vector8s ctor(splat)", "K_V8s_CtorSplat", K_V8s_CtorSplat, 2);
		RunKernelOnly (f, "Vector8s indexer get", "K_V8s_Index", K_V8s_Index, 2);

		Type tu = typeof (Vector8us);
		Type[] tut = { tu, tu };
		Type[] tui = { tu, typeof (int) };
		Type[] tusel = { tu, typeof (ShuffleSel) };

		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us +", "K_V8us_Add", K_V8us_Add, tu.GetMethod ("op_Addition", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us -", "K_V8us_Sub", K_V8us_Sub, tu.GetMethod ("op_Subtraction", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us *", "K_V8us_Mul", K_V8us_Mul, tu.GetMethod ("op_Multiply", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, int, Vector8us> (f, "Vector8us >>", "K_V8us_Shr", K_V8us_Shr, tu.GetMethod ("op_RightShift", tui), V8usA, ShAmt, Bytes, 2);
		RunBinary<Vector8us, int, Vector8us> (f, "Vector8us <<", "K_V8us_Shl", K_V8us_Shl, tu.GetMethod ("op_LeftShift", tui), V8usA, ShAmt, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us &", "K_V8us_And", K_V8us_And, tu.GetMethod ("op_BitwiseAnd", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us |", "K_V8us_Or", K_V8us_Or, tu.GetMethod ("op_BitwiseOr", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us ^", "K_V8us_Xor", K_V8us_Xor, tu.GetMethod ("op_ExclusiveOr", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us.Min", "K_V8us_Min", K_V8us_Min, vo.GetMethod ("Min", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us.Max", "K_V8us_Max", K_V8us_Max, vo.GetMethod ("Max", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us.Average", "K_V8us_Avg", K_V8us_Avg, vo.GetMethod ("Average", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us.CompareEqual", "K_V8us_CmpEq", K_V8us_CmpEq, vo.GetMethod ("CompareEqual", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us.AddWithSaturation", "K_V8us_AddSat", K_V8us_AddSat, vo.GetMethod ("AddWithSaturation", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us.SubtractWithSaturation", "K_V8us_SubSat", K_V8us_SubSat, vo.GetMethod ("SubtractWithSaturation", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us.MultiplyStoreHigh", "K_V8us_MulHi", K_V8us_MulHi, vo.GetMethod ("MultiplyStoreHigh", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us.UnpackLow", "K_V8us_UnpLo", K_V8us_UnpLo, vo.GetMethod ("UnpackLow", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector8us> (f, "Vector8us.UnpackHigh", "K_V8us_UnpHi", K_V8us_UnpHi, vo.GetMethod ("UnpackHigh", tut), V8usA, V8usB, Bytes, 2);
		RunBinary<Vector8us, ShuffleSel, Vector8us> (f, "Vector8us.ShuffleHigh", "K_V8us_ShufHi", K_V8us_ShufHi, vo.GetMethod ("ShuffleHigh", tusel), V8usA, new[] { ShuffleSel.Swap }, Bytes, 2);
		RunBinary<Vector8us, ShuffleSel, Vector8us> (f, "Vector8us.ShuffleLow", "K_V8us_ShufLo", K_V8us_ShufLo, vo.GetMethod ("ShuffleLow", tusel), V8usA, new[] { ShuffleSel.Swap }, Bytes, 2);
		RunBinary<Vector8us, int, Vector8us> (f, "Vector8us.ArithmeticRightShift", "K_V8us_ARShift", K_V8us_ARShift, vo.GetMethod ("ArithmeticRightShift", tui), V8usA, ShAmt, Bytes, 2);
		RunUnary<Vector8us, Vector8s> (f, "Vector8us->Vector8s", "K_V8us_ToV8s", K_V8us_ToV8s, Explicit (tu, typeof (Vector8s)), V8usA, Bytes, 2);
		RunBinary<Vector8us, Vector8us, Vector16b> (f, "Vector8us.SignedPackWithUnsignedSaturation", "K_V8us_PackU", K_V8us_PackU, vo.GetMethod ("SignedPackWithUnsignedSaturation", tut), V8usA, V8usB, Bytes, 1);

		RunKernelOnly (f, "Vector8us ctor(8)", "K_V8us_Ctor8", K_V8us_Ctor8, 2);
		RunKernelOnly (f, "Vector8us ctor(splat)", "K_V8us_CtorSplat", K_V8us_CtorSplat, 2);
		RunKernelOnly (f, "Vector8us indexer get", "K_V8us_Index", K_V8us_Index, 2);
	}

	// ==================== Mono.Simd: Vector16b / Vector16sb ====================

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_Add (int r) { return Bytes (V16bA[r] + V16bB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_Sub (int r) { return Bytes (V16bA[r] - V16bB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_And (int r) { return Bytes (V16bA[r] & V16bB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_Or (int r) { return Bytes (V16bA[r] | V16bB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_Xor (int r) { return Bytes (V16bA[r] ^ V16bB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_Min (int r) { return Bytes (VectorOperations.Min (V16bA[r], V16bB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_Max (int r) { return Bytes (VectorOperations.Max (V16bA[r], V16bB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_Avg (int r) { return Bytes (VectorOperations.Average (V16bA[r], V16bB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_CmpEq (int r) { return Bytes (VectorOperations.CompareEqual (V16bA[r], V16bB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_AddSat (int r) { return Bytes (VectorOperations.AddWithSaturation (V16bA[r], V16bB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_SubSat (int r) { return Bytes (VectorOperations.SubtractWithSaturation (V16bA[r], V16bB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_UnpLo (int r) { return Bytes (VectorOperations.UnpackLow (V16bA[r], V16bB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_UnpHi (int r) { return Bytes (VectorOperations.UnpackHigh (V16bA[r], V16bB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_Mask (int r) { return BitConverter.GetBytes (VectorOperations.ExtractByteMask (V16bA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_Sad (int r) { return Bytes (VectorOperations.SumOfAbsoluteDifferences (V16bA[r], V16sbA[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_ToV16sb (int r) { return Bytes ((Vector16sb) V16bA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)]
	static byte[] K_V16b_Ctor16 (int r)
	{
		return Bytes (new Vector16b (BE[(r + 0) % BE.Length], BE[(r + 1) % BE.Length], BE[(r + 2) % BE.Length], BE[(r + 3) % BE.Length],
		                              BE[(r + 4) % BE.Length], BE[(r + 5) % BE.Length], BE[(r + 6) % BE.Length], BE[(r + 7) % BE.Length],
		                              BE[(r + 8) % BE.Length], BE[(r + 9) % BE.Length], BE[(r + 10) % BE.Length], BE[(r + 11) % BE.Length],
		                              BE[(r + 12) % BE.Length], BE[(r + 13) % BE.Length], BE[(r + 14) % BE.Length], BE[(r + 15) % BE.Length]));
	}
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_CtorSplat (int r) { return Bytes (new Vector16b (BE[r % BE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16b_Index (int r) { return new[] { V16bA[r][r % 16] }; }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_Add (int r) { return Bytes (V16sbA[r] + V16sbB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_Sub (int r) { return Bytes (V16sbA[r] - V16sbB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_And (int r) { return Bytes (V16sbA[r] & V16sbB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_Or (int r) { return Bytes (V16sbA[r] | V16sbB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_Xor (int r) { return Bytes (V16sbA[r] ^ V16sbB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_Min (int r) { return Bytes (VectorOperations.Min (V16sbA[r], V16sbB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_Max (int r) { return Bytes (VectorOperations.Max (V16sbA[r], V16sbB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_CmpEq (int r) { return Bytes (VectorOperations.CompareEqual (V16sbA[r], V16sbB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_CmpGt (int r) { return Bytes (VectorOperations.CompareGreaterThan (V16sbA[r], V16sbB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_AddSat (int r) { return Bytes (VectorOperations.AddWithSaturation (V16sbA[r], V16sbB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_SubSat (int r) { return Bytes (VectorOperations.SubtractWithSaturation (V16sbA[r], V16sbB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_UnpLo (int r) { return Bytes (VectorOperations.UnpackLow (V16sbA[r], V16sbB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_UnpHi (int r) { return Bytes (VectorOperations.UnpackHigh (V16sbA[r], V16sbB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_ToV16b (int r) { return Bytes ((Vector16b) V16sbA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)]
	static byte[] K_V16sb_Ctor16 (int r)
	{
		return Bytes (new Vector16sb (SBE[(r + 0) % SBE.Length], SBE[(r + 1) % SBE.Length], SBE[(r + 2) % SBE.Length], SBE[(r + 3) % SBE.Length],
		                               SBE[(r + 4) % SBE.Length], SBE[(r + 5) % SBE.Length], SBE[(r + 6) % SBE.Length], SBE[(r + 7) % SBE.Length],
		                               SBE[(r + 8) % SBE.Length], SBE[(r + 9) % SBE.Length], SBE[(r + 10) % SBE.Length], SBE[(r + 11) % SBE.Length],
		                               SBE[(r + 12) % SBE.Length], SBE[(r + 13) % SBE.Length], SBE[(r + 14) % SBE.Length], SBE[(r + 15) % SBE.Length]));
	}
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_CtorSplat (int r) { return Bytes (new Vector16sb (SBE[r % SBE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_V16sb_Index (int r) { return new[] { unchecked ((byte) V16sbA[r][r % 16]) }; }

	static void CheckVector16b ()
	{
		string f = "Mono.Simd";
		Type t = typeof (Vector16b);
		Type[] tt = { t, t };
		Type vo = typeof (VectorOperations);

		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b +", "K_V16b_Add", K_V16b_Add, t.GetMethod ("op_Addition", tt), V16bA, V16bB, Bytes, 1);
		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b -", "K_V16b_Sub", K_V16b_Sub, t.GetMethod ("op_Subtraction", tt), V16bA, V16bB, Bytes, 1);
		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b &", "K_V16b_And", K_V16b_And, t.GetMethod ("op_BitwiseAnd", tt), V16bA, V16bB, Bytes, 1);
		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b |", "K_V16b_Or", K_V16b_Or, t.GetMethod ("op_BitwiseOr", tt), V16bA, V16bB, Bytes, 1);
		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b ^", "K_V16b_Xor", K_V16b_Xor, t.GetMethod ("op_ExclusiveOr", tt), V16bA, V16bB, Bytes, 1);
		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b.Min", "K_V16b_Min", K_V16b_Min, vo.GetMethod ("Min", tt), V16bA, V16bB, Bytes, 1);
		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b.Max", "K_V16b_Max", K_V16b_Max, vo.GetMethod ("Max", tt), V16bA, V16bB, Bytes, 1);
		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b.Average", "K_V16b_Avg", K_V16b_Avg, vo.GetMethod ("Average", tt), V16bA, V16bB, Bytes, 1);
		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b.CompareEqual", "K_V16b_CmpEq", K_V16b_CmpEq, vo.GetMethod ("CompareEqual", tt), V16bA, V16bB, Bytes, 1);
		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b.AddWithSaturation", "K_V16b_AddSat", K_V16b_AddSat, vo.GetMethod ("AddWithSaturation", tt), V16bA, V16bB, Bytes, 1);
		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b.SubtractWithSaturation", "K_V16b_SubSat", K_V16b_SubSat, vo.GetMethod ("SubtractWithSaturation", tt), V16bA, V16bB, Bytes, 1);
		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b.UnpackLow", "K_V16b_UnpLo", K_V16b_UnpLo, vo.GetMethod ("UnpackLow", tt), V16bA, V16bB, Bytes, 1);
		RunBinary<Vector16b, Vector16b, Vector16b> (f, "Vector16b.UnpackHigh", "K_V16b_UnpHi", K_V16b_UnpHi, vo.GetMethod ("UnpackHigh", tt), V16bA, V16bB, Bytes, 1);
		RunUnary<Vector16b, int> (f, "Vector16b.ExtractByteMask", "K_V16b_Mask", K_V16b_Mask, vo.GetMethod ("ExtractByteMask", new[] { t }), V16bA, BitConverter.GetBytes, 4);
		RunBinary<Vector16b, Vector16sb, Vector8us> (f, "Vector16b.SumOfAbsoluteDifferences", "K_V16b_Sad", K_V16b_Sad,
			vo.GetMethod ("SumOfAbsoluteDifferences", new[] { t, typeof (Vector16sb) }), V16bA, V16sbA, Bytes, 2);
		RunUnary<Vector16b, Vector16sb> (f, "Vector16b->Vector16sb", "K_V16b_ToV16sb", K_V16b_ToV16sb, Explicit (t, typeof (Vector16sb)), V16bA, Bytes, 1);

		RunKernelOnly (f, "Vector16b ctor(16)", "K_V16b_Ctor16", K_V16b_Ctor16, 1);
		RunKernelOnly (f, "Vector16b ctor(splat)", "K_V16b_CtorSplat", K_V16b_CtorSplat, 1);
		RunKernelOnly (f, "Vector16b indexer get", "K_V16b_Index", K_V16b_Index, 1);

		Type ts = typeof (Vector16sb);
		Type[] tst = { ts, ts };

		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb +", "K_V16sb_Add", K_V16sb_Add, ts.GetMethod ("op_Addition", tst), V16sbA, V16sbB, Bytes, 1);
		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb -", "K_V16sb_Sub", K_V16sb_Sub, ts.GetMethod ("op_Subtraction", tst), V16sbA, V16sbB, Bytes, 1);
		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb &", "K_V16sb_And", K_V16sb_And, ts.GetMethod ("op_BitwiseAnd", tst), V16sbA, V16sbB, Bytes, 1);
		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb |", "K_V16sb_Or", K_V16sb_Or, ts.GetMethod ("op_BitwiseOr", tst), V16sbA, V16sbB, Bytes, 1);
		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb ^", "K_V16sb_Xor", K_V16sb_Xor, ts.GetMethod ("op_ExclusiveOr", tst), V16sbA, V16sbB, Bytes, 1);
		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb.Min", "K_V16sb_Min", K_V16sb_Min, vo.GetMethod ("Min", tst), V16sbA, V16sbB, Bytes, 1);
		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb.Max", "K_V16sb_Max", K_V16sb_Max, vo.GetMethod ("Max", tst), V16sbA, V16sbB, Bytes, 1);
		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb.CompareEqual", "K_V16sb_CmpEq", K_V16sb_CmpEq, vo.GetMethod ("CompareEqual", tst), V16sbA, V16sbB, Bytes, 1);
		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb.CompareGreaterThan", "K_V16sb_CmpGt", K_V16sb_CmpGt, vo.GetMethod ("CompareGreaterThan", tst), V16sbA, V16sbB, Bytes, 1);
		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb.AddWithSaturation", "K_V16sb_AddSat", K_V16sb_AddSat, vo.GetMethod ("AddWithSaturation", tst), V16sbA, V16sbB, Bytes, 1);
		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb.SubtractWithSaturation", "K_V16sb_SubSat", K_V16sb_SubSat, vo.GetMethod ("SubtractWithSaturation", tst), V16sbA, V16sbB, Bytes, 1);
		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb.UnpackLow", "K_V16sb_UnpLo", K_V16sb_UnpLo, vo.GetMethod ("UnpackLow", tst), V16sbA, V16sbB, Bytes, 1);
		RunBinary<Vector16sb, Vector16sb, Vector16sb> (f, "Vector16sb.UnpackHigh", "K_V16sb_UnpHi", K_V16sb_UnpHi, vo.GetMethod ("UnpackHigh", tst), V16sbA, V16sbB, Bytes, 1);
		RunUnary<Vector16sb, Vector16b> (f, "Vector16sb->Vector16b", "K_V16sb_ToV16b", K_V16sb_ToV16b, Explicit (ts, typeof (Vector16b)), V16sbA, Bytes, 1);

		RunKernelOnly (f, "Vector16sb ctor(16)", "K_V16sb_Ctor16", K_V16sb_Ctor16, 1);
		RunKernelOnly (f, "Vector16sb ctor(splat)", "K_V16sb_CtorSplat", K_V16sb_CtorSplat, 1);
		RunKernelOnly (f, "Vector16sb indexer get", "K_V16sb_Index", K_V16sb_Index, 1);
	}

	// ==================== System.Numerics.Vector4 ====================

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Add (int r) { return Bytes (SNV4A[r] + SNV4B[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Sub (int r) { return Bytes (SNV4A[r] - SNV4B[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Mul (int r) { return Bytes (SNV4A[r] * SNV4B[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_MulScalar (int r) { return Bytes (SNV4A[r] * FA (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Div (int r) { return Bytes (SNV4A[r] / SNV4B[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_DivScalar (int r) { return Bytes (SNV4A[r] / FA (r)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Neg (int r) { return Bytes (-SNV4A[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Min (int r) { return Bytes (System.Numerics.Vector4.Min (SNV4A[r], SNV4B[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Max (int r) { return Bytes (System.Numerics.Vector4.Max (SNV4A[r], SNV4B[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Abs (int r) { return Bytes (System.Numerics.Vector4.Abs (SNV4A[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Sqrt (int r) { return Bytes (System.Numerics.Vector4.SquareRoot (SNV4A[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Dot (int r) { return BitConverter.GetBytes (System.Numerics.Vector4.Dot (SNV4A[r], SNV4B[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Dist (int r) { return BitConverter.GetBytes (System.Numerics.Vector4.Distance (SNV4A[r], SNV4B[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Len (int r) { return BitConverter.GetBytes (SNV4A[r].Length ()); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Norm (int r) { return Bytes (System.Numerics.Vector4.Normalize (SNV4A[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Clamp (int r) { return Bytes (System.Numerics.Vector4.Clamp (SNV4A[r], -System.Numerics.Vector4.One, System.Numerics.Vector4.One)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Lerp (int r) { return Bytes (System.Numerics.Vector4.Lerp (SNV4A[r], SNV4B[r], FA (r) == 0f ? 0f : 0.25f)); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Eq (int r) { return new[] { (byte) (SNV4A[r] == SNV4B[r] ? 1 : 0) }; }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_Ctor4 (int r) { return Bytes (new System.Numerics.Vector4 (FE[r % FE.Length], FE[(r + 1) % FE.Length], FE[(r + 2) % FE.Length], FE[(r + 3) % FE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_SNV4_CtorSplat (int r) { return Bytes (new System.Numerics.Vector4 (FE[r % FE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)]
	static byte[] K_SNV4_CopyTo (int r)
	{
		float[] a = new float[4];
		SNV4A[r].CopyTo (a);
		byte[] result = new byte[16];
		Buffer.BlockCopy (a, 0, result, 0, 16);
		return result;
	}

	static void CheckVector4 ()
	{
		string f = "System.Numerics.Vector4";
		Type t = typeof (System.Numerics.Vector4);
		Type[] tt = { t, t };
		Type[] tf = { t, typeof (float) };
		Type[] ft = { typeof (float), t };

		RunBinary<System.Numerics.Vector4, System.Numerics.Vector4, System.Numerics.Vector4> (f, "Vector4 +", "K_SNV4_Add", K_SNV4_Add, t.GetMethod ("op_Addition", tt), SNV4A, SNV4B, Bytes, 4);
		RunBinary<System.Numerics.Vector4, System.Numerics.Vector4, System.Numerics.Vector4> (f, "Vector4 -", "K_SNV4_Sub", K_SNV4_Sub, t.GetMethod ("op_Subtraction", tt), SNV4A, SNV4B, Bytes, 4);
		RunBinary<System.Numerics.Vector4, System.Numerics.Vector4, System.Numerics.Vector4> (f, "Vector4 *", "K_SNV4_Mul", K_SNV4_Mul, t.GetMethod ("op_Multiply", tt), SNV4A, SNV4B, Bytes, 4);
		RunBinary<System.Numerics.Vector4, float, System.Numerics.Vector4> (f, "Vector4 * scalar", "K_SNV4_MulScalar", K_SNV4_MulScalar, t.GetMethod ("op_Multiply", tf), SNV4A, FE, Bytes, 4);
		RunBinary<System.Numerics.Vector4, System.Numerics.Vector4, System.Numerics.Vector4> (f, "Vector4 /", "K_SNV4_Div", K_SNV4_Div, t.GetMethod ("op_Division", tt), SNV4A, SNV4B, Bytes, 4);
		RunBinary<System.Numerics.Vector4, float, System.Numerics.Vector4> (f, "Vector4 / scalar", "K_SNV4_DivScalar", K_SNV4_DivScalar, t.GetMethod ("op_Division", tf), SNV4A, FE, Bytes, 4);
		RunUnary<System.Numerics.Vector4, System.Numerics.Vector4> (f, "-Vector4", "K_SNV4_Neg", K_SNV4_Neg, t.GetMethod ("op_UnaryNegation", new[] { t }), SNV4A, Bytes, 4);
		RunBinary<System.Numerics.Vector4, System.Numerics.Vector4, System.Numerics.Vector4> (f, "Vector4.Min", "K_SNV4_Min", K_SNV4_Min, t.GetMethod ("Min", tt), SNV4A, SNV4B, Bytes, 4);
		RunBinary<System.Numerics.Vector4, System.Numerics.Vector4, System.Numerics.Vector4> (f, "Vector4.Max", "K_SNV4_Max", K_SNV4_Max, t.GetMethod ("Max", tt), SNV4A, SNV4B, Bytes, 4);
		RunUnary<System.Numerics.Vector4, System.Numerics.Vector4> (f, "Vector4.Abs", "K_SNV4_Abs", K_SNV4_Abs, t.GetMethod ("Abs", new[] { t }), SNV4A, Bytes, 4);
		RunUnary<System.Numerics.Vector4, System.Numerics.Vector4> (f, "Vector4.SquareRoot", "K_SNV4_Sqrt", K_SNV4_Sqrt, t.GetMethod ("SquareRoot", new[] { t }), SNV4A, Bytes, 4);
		RunBinary<System.Numerics.Vector4, System.Numerics.Vector4, float> (f, "Vector4.Dot", "K_SNV4_Dot", K_SNV4_Dot, t.GetMethod ("Dot", tt), SNV4A, SNV4B, BitConverter.GetBytes, 4, floatRelax: true);
		RunBinary<System.Numerics.Vector4, System.Numerics.Vector4, float> (f, "Vector4.Distance", "K_SNV4_Dist", K_SNV4_Dist, t.GetMethod ("Distance", tt), SNV4A, SNV4B, BitConverter.GetBytes, 4, floatRelax: true);
		// Length () is an instance method, and an open-instance delegate over a
		// struct method fails to bind on this runtime, so this checks only the
		// three arms that do not go through a delegate.
		RunKernelOnly (f, "Vector4.Length", "K_SNV4_Len", K_SNV4_Len, 4);
		RunUnary<System.Numerics.Vector4, System.Numerics.Vector4> (f, "Vector4.Normalize", "K_SNV4_Norm", K_SNV4_Norm, t.GetMethod ("Normalize", new[] { t }), SNV4A, Bytes, 4, floatRelax: true);

		RunKernelOnly (f, "Vector4.Clamp", "K_SNV4_Clamp", K_SNV4_Clamp, 4);
		RunKernelOnly (f, "Vector4.Lerp", "K_SNV4_Lerp", K_SNV4_Lerp, 4);
		RunKernelOnly (f, "Vector4 ==", "K_SNV4_Eq", K_SNV4_Eq, 1);
		RunKernelOnly (f, "Vector4 ctor(4)", "K_SNV4_Ctor4", K_SNV4_Ctor4, 4);
		RunKernelOnly (f, "Vector4 ctor(splat)", "K_SNV4_CtorSplat", K_SNV4_CtorSplat, 4);
		RunKernelOnly (f, "Vector4.CopyTo", "K_SNV4_CopyTo", K_SNV4_CopyTo, 4);
	}

	// ==================== System.Numerics.Vector<T> ====================

	static byte[] ScalarBytes (byte v) { return new[] { v }; }
	static byte[] ScalarBytes (sbyte v) { return new[] { unchecked ((byte) v) }; }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Add (int r) { return Bytes (VFA[r] + VFB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Sub (int r) { return Bytes (VFA[r] - VFB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Mul (int r) { return Bytes (VFA[r] * VFB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_And (int r) { return Bytes (VFA[r] & VFB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Or (int r) { return Bytes (VFA[r] | VFB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Xor (int r) { return Bytes (VFA[r] ^ VFB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Neg (int r) { return Bytes (-VFA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Eq (int r) { return Bytes (System.Numerics.Vector.Equals<float> (VFA[r], VFB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Lt (int r) { return Bytes (System.Numerics.Vector.LessThan<float> (VFA[r], VFB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Min (int r) { return Bytes (System.Numerics.Vector.Min<float> (VFA[r], VFB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Max (int r) { return Bytes (System.Numerics.Vector.Max<float> (VFA[r], VFB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Dot (int r) { return BitConverter.GetBytes (System.Numerics.Vector.Dot<float> (VFA[r], VFB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Ctor (int r) { return Bytes (new Vector<float> (FA (r))); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VF_Index (int r) { return BitConverter.GetBytes (VFA[r][r % Vector<float>.Count]); }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VD_Add (int r) { return Bytes (VDA[r] + VDB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VD_Sub (int r) { return Bytes (VDA[r] - VDB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VD_Mul (int r) { return Bytes (VDA[r] * VDB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VD_And (int r) { return Bytes (VDA[r] & VDB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VD_Or (int r) { return Bytes (VDA[r] | VDB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VD_Xor (int r) { return Bytes (VDA[r] ^ VDB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VD_Neg (int r) { return Bytes (-VDA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VD_Eq (int r) { return Bytes (System.Numerics.Vector.Equals<double> (VDA[r], VDB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VD_Lt (int r) { return Bytes (System.Numerics.Vector.LessThan<double> (VDA[r], VDB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VD_Min (int r) { return Bytes (System.Numerics.Vector.Min<double> (VDA[r], VDB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VD_Max (int r) { return Bytes (System.Numerics.Vector.Max<double> (VDA[r], VDB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VD_Dot (int r) { return BitConverter.GetBytes (System.Numerics.Vector.Dot<double> (VDA[r], VDB[r])); }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Add (int r) { return Bytes (VIA[r] + VIB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Sub (int r) { return Bytes (VIA[r] - VIB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Mul (int r) { return Bytes (VIA[r] * VIB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_And (int r) { return Bytes (VIA[r] & VIB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Or (int r) { return Bytes (VIA[r] | VIB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Xor (int r) { return Bytes (VIA[r] ^ VIB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Neg (int r) { return Bytes (-VIA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Eq (int r) { return Bytes (System.Numerics.Vector.Equals<int> (VIA[r], VIB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Lt (int r) { return Bytes (System.Numerics.Vector.LessThan<int> (VIA[r], VIB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Min (int r) { return Bytes (System.Numerics.Vector.Min<int> (VIA[r], VIB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Max (int r) { return Bytes (System.Numerics.Vector.Max<int> (VIA[r], VIB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Dot (int r) { return BitConverter.GetBytes (System.Numerics.Vector.Dot<int> (VIA[r], VIB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Abs (int r) { return Bytes (System.Numerics.Vector.Abs<int> (VIAabs[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Ctor (int r) { return Bytes (new Vector<int> (IE[r % IE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VI_Index (int r) { return BitConverter.GetBytes (VIA[r][r % Vector<int>.Count]); }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUI_Add (int r) { return Bytes (VUIA[r] + VUIB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUI_Sub (int r) { return Bytes (VUIA[r] - VUIB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUI_Mul (int r) { return Bytes (VUIA[r] * VUIB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUI_And (int r) { return Bytes (VUIA[r] & VUIB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUI_Or (int r) { return Bytes (VUIA[r] | VUIB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUI_Xor (int r) { return Bytes (VUIA[r] ^ VUIB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUI_Neg (int r) { return Bytes (-VUIA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUI_Eq (int r) { return Bytes (System.Numerics.Vector.Equals<uint> (VUIA[r], VUIB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUI_Lt (int r) { return Bytes (System.Numerics.Vector.LessThan<uint> (VUIA[r], VUIB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUI_Min (int r) { return Bytes (System.Numerics.Vector.Min<uint> (VUIA[r], VUIB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUI_Max (int r) { return Bytes (System.Numerics.Vector.Max<uint> (VUIA[r], VUIB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUI_Dot (int r) { return BitConverter.GetBytes (System.Numerics.Vector.Dot<uint> (VUIA[r], VUIB[r])); }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_Add (int r) { return Bytes (VLA[r] + VLB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_Sub (int r) { return Bytes (VLA[r] - VLB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_Mul (int r) { return Bytes (VLA[r] * VLB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_And (int r) { return Bytes (VLA[r] & VLB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_Or (int r) { return Bytes (VLA[r] | VLB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_Xor (int r) { return Bytes (VLA[r] ^ VLB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_Neg (int r) { return Bytes (-VLA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_Eq (int r) { return Bytes (System.Numerics.Vector.Equals<long> (VLA[r], VLB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_Lt (int r) { return Bytes (System.Numerics.Vector.LessThan<long> (VLA[r], VLB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_Min (int r) { return Bytes (System.Numerics.Vector.Min<long> (VLA[r], VLB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_Max (int r) { return Bytes (System.Numerics.Vector.Max<long> (VLA[r], VLB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_Dot (int r) { return BitConverter.GetBytes (System.Numerics.Vector.Dot<long> (VLA[r], VLB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VL_Abs (int r) { return Bytes (System.Numerics.Vector.Abs<long> (VLAabs[r])); }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUL_Add (int r) { return Bytes (VULA[r] + VULB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUL_Sub (int r) { return Bytes (VULA[r] - VULB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUL_Mul (int r) { return Bytes (VULA[r] * VULB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUL_And (int r) { return Bytes (VULA[r] & VULB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUL_Or (int r) { return Bytes (VULA[r] | VULB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUL_Xor (int r) { return Bytes (VULA[r] ^ VULB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUL_Neg (int r) { return Bytes (-VULA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUL_Eq (int r) { return Bytes (System.Numerics.Vector.Equals<ulong> (VULA[r], VULB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUL_Lt (int r) { return Bytes (System.Numerics.Vector.LessThan<ulong> (VULA[r], VULB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUL_Min (int r) { return Bytes (System.Numerics.Vector.Min<ulong> (VULA[r], VULB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUL_Max (int r) { return Bytes (System.Numerics.Vector.Max<ulong> (VULA[r], VULB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUL_Dot (int r) { return BitConverter.GetBytes (System.Numerics.Vector.Dot<ulong> (VULA[r], VULB[r])); }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_Add (int r) { return Bytes (VSA[r] + VSB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_Sub (int r) { return Bytes (VSA[r] - VSB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_Mul (int r) { return Bytes (VSA[r] * VSB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_And (int r) { return Bytes (VSA[r] & VSB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_Or (int r) { return Bytes (VSA[r] | VSB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_Xor (int r) { return Bytes (VSA[r] ^ VSB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_Neg (int r) { return Bytes (-VSA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_Eq (int r) { return Bytes (System.Numerics.Vector.Equals<short> (VSA[r], VSB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_Lt (int r) { return Bytes (System.Numerics.Vector.LessThan<short> (VSA[r], VSB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_Min (int r) { return Bytes (System.Numerics.Vector.Min<short> (VSA[r], VSB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_Max (int r) { return Bytes (System.Numerics.Vector.Max<short> (VSA[r], VSB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_Dot (int r) { return BitConverter.GetBytes (System.Numerics.Vector.Dot<short> (VSA[r], VSB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VS_Abs (int r) { return Bytes (System.Numerics.Vector.Abs<short> (VSAabs[r])); }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUS_Add (int r) { return Bytes (VUSA[r] + VUSB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUS_Sub (int r) { return Bytes (VUSA[r] - VUSB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUS_Mul (int r) { return Bytes (VUSA[r] * VUSB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUS_And (int r) { return Bytes (VUSA[r] & VUSB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUS_Or (int r) { return Bytes (VUSA[r] | VUSB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUS_Xor (int r) { return Bytes (VUSA[r] ^ VUSB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUS_Neg (int r) { return Bytes (-VUSA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUS_Eq (int r) { return Bytes (System.Numerics.Vector.Equals<ushort> (VUSA[r], VUSB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUS_Lt (int r) { return Bytes (System.Numerics.Vector.LessThan<ushort> (VUSA[r], VUSB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUS_Min (int r) { return Bytes (System.Numerics.Vector.Min<ushort> (VUSA[r], VUSB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUS_Max (int r) { return Bytes (System.Numerics.Vector.Max<ushort> (VUSA[r], VUSB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VUS_Dot (int r) { return BitConverter.GetBytes (System.Numerics.Vector.Dot<ushort> (VUSA[r], VUSB[r])); }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Add (int r) { return Bytes (VBA[r] + VBB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Sub (int r) { return Bytes (VBA[r] - VBB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Mul (int r) { return Bytes (VBA[r] * VBB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_And (int r) { return Bytes (VBA[r] & VBB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Or (int r) { return Bytes (VBA[r] | VBB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Xor (int r) { return Bytes (VBA[r] ^ VBB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Neg (int r) { return Bytes (-VBA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Eq (int r) { return Bytes (System.Numerics.Vector.Equals<byte> (VBA[r], VBB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Lt (int r) { return Bytes (System.Numerics.Vector.LessThan<byte> (VBA[r], VBB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Min (int r) { return Bytes (System.Numerics.Vector.Min<byte> (VBA[r], VBB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Max (int r) { return Bytes (System.Numerics.Vector.Max<byte> (VBA[r], VBB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Dot (int r) { return ScalarBytes (System.Numerics.Vector.Dot<byte> (VBA[r], VBB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Ctor (int r) { return Bytes (new Vector<byte> (BE[r % BE.Length])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VB_Index (int r) { return new[] { VBA[r][r % Vector<byte>.Count] }; }

	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_Add (int r) { return Bytes (VSBA[r] + VSBB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_Sub (int r) { return Bytes (VSBA[r] - VSBB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_Mul (int r) { return Bytes (VSBA[r] * VSBB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_And (int r) { return Bytes (VSBA[r] & VSBB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_Or (int r) { return Bytes (VSBA[r] | VSBB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_Xor (int r) { return Bytes (VSBA[r] ^ VSBB[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_Neg (int r) { return Bytes (-VSBA[r]); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_Eq (int r) { return Bytes (System.Numerics.Vector.Equals<sbyte> (VSBA[r], VSBB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_Lt (int r) { return Bytes (System.Numerics.Vector.LessThan<sbyte> (VSBA[r], VSBB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_Min (int r) { return Bytes (System.Numerics.Vector.Min<sbyte> (VSBA[r], VSBB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_Max (int r) { return Bytes (System.Numerics.Vector.Max<sbyte> (VSBA[r], VSBB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_Dot (int r) { return ScalarBytes (System.Numerics.Vector.Dot<sbyte> (VSBA[r], VSBB[r])); }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VSB_Abs (int r) { return Bytes (System.Numerics.Vector.Abs<sbyte> (VSBAabs[r])); }

	// int.MinValue pairs with 0 at every row here, not with -1: that division
	// overflows rather than raising DivideByZeroException, which is a
	// different arm CheckIntDivByZero () does not cover.
	static Vector<int>[] VIntDivA = MakeVecTRows<int> (new[] { 6, -7, 100, int.MinValue, 50 }, 0);
	static Vector<int>[] VIntDivB = MakeVecTRows<int> (new[] { 2, 3, 7, 0, 5 }, 0);
	[MethodImpl (MethodImplOptions.NoInlining)] static byte[] K_VIntDiv (int r) { return Bytes (VIntDivA[r] / VIntDivB[r]); }

	static void CheckOneVecT<T> (string typeName, string suf,
	                              Func<int, byte[]> kAdd, Func<int, byte[]> kSub, Func<int, byte[]> kMul,
	                              Func<int, byte[]> kAnd, Func<int, byte[]> kOr, Func<int, byte[]> kXor,
	                              Func<int, byte[]> kNeg, Func<int, byte[]> kEq, Func<int, byte[]> kLt,
	                              Func<int, byte[]> kMin, Func<int, byte[]> kMax, Func<int, byte[]> kDot,
	                              Vector<T>[] a, Vector<T>[] b, Func<Vector<T>, byte[]> toBytes, Func<T, byte[]> toBytesScalar, int elemSize) where T : struct
	{
		string f = "System.Numerics.Vector<T>";
		Type t = typeof (Vector<T>);
		Type[] tt = { t, t };

		RunBinary<Vector<T>, Vector<T>, Vector<T>> (f, typeName + " +", "K_" + suf + "_Add", kAdd, t.GetMethod ("op_Addition", tt), a, b, toBytes, elemSize);
		RunBinary<Vector<T>, Vector<T>, Vector<T>> (f, typeName + " -", "K_" + suf + "_Sub", kSub, t.GetMethod ("op_Subtraction", tt), a, b, toBytes, elemSize);
		RunBinary<Vector<T>, Vector<T>, Vector<T>> (f, typeName + " *", "K_" + suf + "_Mul", kMul, t.GetMethod ("op_Multiply", tt), a, b, toBytes, elemSize);
		RunBinary<Vector<T>, Vector<T>, Vector<T>> (f, typeName + " &", "K_" + suf + "_And", kAnd, t.GetMethod ("op_BitwiseAnd", tt), a, b, toBytes, elemSize);
		RunBinary<Vector<T>, Vector<T>, Vector<T>> (f, typeName + " |", "K_" + suf + "_Or", kOr, t.GetMethod ("op_BitwiseOr", tt), a, b, toBytes, elemSize);
		RunBinary<Vector<T>, Vector<T>, Vector<T>> (f, typeName + " ^", "K_" + suf + "_Xor", kXor, t.GetMethod ("op_ExclusiveOr", tt), a, b, toBytes, elemSize);
		RunUnary<Vector<T>, Vector<T>> (f, "-" + typeName, "K_" + suf + "_Neg", kNeg, t.GetMethod ("op_UnaryNegation", new[] { t }), a, toBytes, elemSize);
		RunBinary<Vector<T>, Vector<T>, Vector<T>> (f, typeName + " Vector.Equals", "K_" + suf + "_Eq", kEq, VecGeneric ("Equals", typeof (T)), a, b, toBytes, elemSize);
		RunBinary<Vector<T>, Vector<T>, Vector<T>> (f, typeName + " Vector.LessThan", "K_" + suf + "_Lt", kLt, VecGeneric ("LessThan", typeof (T)), a, b, toBytes, elemSize);
		RunBinary<Vector<T>, Vector<T>, Vector<T>> (f, typeName + " Vector.Min", "K_" + suf + "_Min", kMin, VecGeneric ("Min", typeof (T)), a, b, toBytes, elemSize);
		RunBinary<Vector<T>, Vector<T>, Vector<T>> (f, typeName + " Vector.Max", "K_" + suf + "_Max", kMax, VecGeneric ("Max", typeof (T)), a, b, toBytes, elemSize);
		bool dotIsFloating = typeof (T) == typeof (float) || typeof (T) == typeof (double);
		RunBinary<Vector<T>, Vector<T>, T> (f, typeName + " Vector.Dot", "K_" + suf + "_Dot", kDot, VecGeneric ("Dot", typeof (T)), a, b, toBytesScalar, elemSize, floatRelax: dotIsFloating);
	}

	static void CheckVectorT ()
	{
		string f = "System.Numerics.Vector<T>";

		CheckOneVecT<float> ("Vector<float>", "VF", K_VF_Add, K_VF_Sub, K_VF_Mul, K_VF_And, K_VF_Or, K_VF_Xor,
			K_VF_Neg, K_VF_Eq, K_VF_Lt, K_VF_Min, K_VF_Max, K_VF_Dot, VFA, VFB, Bytes, BitConverter.GetBytes, 4);
		CheckOneVecT<double> ("Vector<double>", "VD", K_VD_Add, K_VD_Sub, K_VD_Mul, K_VD_And, K_VD_Or, K_VD_Xor,
			K_VD_Neg, K_VD_Eq, K_VD_Lt, K_VD_Min, K_VD_Max, K_VD_Dot, VDA, VDB, Bytes, BitConverter.GetBytes, 8);
		CheckOneVecT<int> ("Vector<int>", "VI", K_VI_Add, K_VI_Sub, K_VI_Mul, K_VI_And, K_VI_Or, K_VI_Xor,
			K_VI_Neg, K_VI_Eq, K_VI_Lt, K_VI_Min, K_VI_Max, K_VI_Dot, VIA, VIB, Bytes, BitConverter.GetBytes, 4);
		CheckOneVecT<uint> ("Vector<uint>", "VUI", K_VUI_Add, K_VUI_Sub, K_VUI_Mul, K_VUI_And, K_VUI_Or, K_VUI_Xor,
			K_VUI_Neg, K_VUI_Eq, K_VUI_Lt, K_VUI_Min, K_VUI_Max, K_VUI_Dot, VUIA, VUIB, Bytes, BitConverter.GetBytes, 4);
		CheckOneVecT<long> ("Vector<long>", "VL", K_VL_Add, K_VL_Sub, K_VL_Mul, K_VL_And, K_VL_Or, K_VL_Xor,
			K_VL_Neg, K_VL_Eq, K_VL_Lt, K_VL_Min, K_VL_Max, K_VL_Dot, VLA, VLB, Bytes, BitConverter.GetBytes, 8);
		CheckOneVecT<ulong> ("Vector<ulong>", "VUL", K_VUL_Add, K_VUL_Sub, K_VUL_Mul, K_VUL_And, K_VUL_Or, K_VUL_Xor,
			K_VUL_Neg, K_VUL_Eq, K_VUL_Lt, K_VUL_Min, K_VUL_Max, K_VUL_Dot, VULA, VULB, Bytes, BitConverter.GetBytes, 8);
		CheckOneVecT<short> ("Vector<short>", "VS", K_VS_Add, K_VS_Sub, K_VS_Mul, K_VS_And, K_VS_Or, K_VS_Xor,
			K_VS_Neg, K_VS_Eq, K_VS_Lt, K_VS_Min, K_VS_Max, K_VS_Dot, VSA, VSB, Bytes, BitConverter.GetBytes, 2);
		CheckOneVecT<ushort> ("Vector<ushort>", "VUS", K_VUS_Add, K_VUS_Sub, K_VUS_Mul, K_VUS_And, K_VUS_Or, K_VUS_Xor,
			K_VUS_Neg, K_VUS_Eq, K_VUS_Lt, K_VUS_Min, K_VUS_Max, K_VUS_Dot, VUSA, VUSB, Bytes, BitConverter.GetBytes, 2);
		CheckOneVecT<byte> ("Vector<byte>", "VB", K_VB_Add, K_VB_Sub, K_VB_Mul, K_VB_And, K_VB_Or, K_VB_Xor,
			K_VB_Neg, K_VB_Eq, K_VB_Lt, K_VB_Min, K_VB_Max, K_VB_Dot, VBA, VBB, Bytes, ScalarBytes, 1);
		CheckOneVecT<sbyte> ("Vector<sbyte>", "VSB", K_VSB_Add, K_VSB_Sub, K_VSB_Mul, K_VSB_And, K_VSB_Or, K_VSB_Xor,
			K_VSB_Neg, K_VSB_Eq, K_VSB_Lt, K_VSB_Min, K_VSB_Max, K_VSB_Dot, VSBA, VSBB, Bytes, ScalarBytes, 1);

		RunUnary<Vector<int>, Vector<int>> (f, "Vector<int>.Abs", "K_VI_Abs", K_VI_Abs, VecGeneric ("Abs", typeof (int)), VIAabs, Bytes, 4);
		RunUnary<Vector<long>, Vector<long>> (f, "Vector<long>.Abs", "K_VL_Abs", K_VL_Abs, VecGeneric ("Abs", typeof (long)), VLAabs, Bytes, 8);
		RunUnary<Vector<short>, Vector<short>> (f, "Vector<short>.Abs", "K_VS_Abs", K_VS_Abs, VecGeneric ("Abs", typeof (short)), VSAabs, Bytes, 2);
		RunUnary<Vector<sbyte>, Vector<sbyte>> (f, "Vector<sbyte>.Abs", "K_VSB_Abs", K_VSB_Abs, VecGeneric ("Abs", typeof (sbyte)), VSBAabs, Bytes, 1);

		RunKernelOnly (f, "Vector<float> ctor(T)", "K_VF_Ctor", K_VF_Ctor, 4);
		RunKernelOnly (f, "Vector<float> indexer get", "K_VF_Index", K_VF_Index, 4);
		RunKernelOnly (f, "Vector<int> ctor(T)", "K_VI_Ctor", K_VI_Ctor, 4);
		RunKernelOnly (f, "Vector<int> indexer get", "K_VI_Index", K_VI_Index, 4);
		RunKernelOnly (f, "Vector<byte> ctor(T)", "K_VB_Ctor", K_VB_Ctor, 1);
		RunKernelOnly (f, "Vector<byte> indexer get", "K_VB_Index", K_VB_Index, 1);
	}

	// A lane divisor of zero makes op_Division throw. "Threw" is a result the
	// four arms have to agree on exactly as they agree on a value, so this is
	// answered through the same Report () path with a null standing for it.
	static void CheckIntDivByZero ()
	{
		string family = "System.Numerics.Vector<T>";
		string op = "Vector<int> /";
		MethodInfo kernelMI = Kernel ("K_VIntDiv");
		MethodInfo body = typeof (Vector<int>).GetMethod ("op_Division", new[] { typeof (Vector<int>), typeof (Vector<int>) });
		var del = (Func<Vector<int>, Vector<int>, Vector<int>>) Delegate.CreateDelegate (
			typeof (Func<Vector<int>, Vector<int>, Vector<int>>), body);

		var interp = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++) { int rr = r; interp[r] = Try (() => K_VIntDiv (rr)); }

		Promote (kernelMI, TIER1);
		var t1 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++) { int rr = r; t1[r] = Try (() => K_VIntDiv (rr)); }

		Promote (kernelMI, TIER2);
		var t2 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++) { int rr = r; t2[r] = Try (() => K_VIntDiv (rr)); }

		Promote (body, TIER1);
		var d1 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++) { int rr = r; d1[r] = Try (() => Bytes (del (VIntDivA[rr % VIntDivA.Length], VIntDivB[rr % VIntDivB.Length]))); }

		Promote (body, TIER2);
		var d2 = new byte[ROWS][];
		for (int r = 0; r < ROWS; r++) { int rr = r; d2[r] = Try (() => Bytes (del (VIntDivA[rr % VIntDivA.Length], VIntDivB[rr % VIntDivB.Length]))); }

		for (int r = 0; r < ROWS; r++)
			Report (family, op, r, interp[r], AllArms, new[] { t1[r], t2[r], d1[r], d2[r] }, 4);
	}

	static int Main ()
	{
		CheckVector4f ();
		CheckVector4i ();
		CheckVector4ui ();
		CheckVector2l ();
		CheckVector2d ();
		CheckVector8s ();
		CheckVector16b ();
		CheckVector4 ();
		CheckVectorT ();
		CheckIntDivByZero ();

		Console.WriteLine (comparisons + " comparisons, " + mismatches + " mismatches, " +
			promoteFailures + " PromoteNow failures");

		return (mismatches == 0 && promoteFailures == 0) ? 0 : 1;
	}
}
