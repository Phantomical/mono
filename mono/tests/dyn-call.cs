using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

/*
 * Where an interpreted caller puts each argument of a call into compiled code.
 *
 * plan_dyn_call () (mono/llvm/arch/amd64/dyn-call.cpp) states the convention
 * for one prototype, and dyn-call-thunk.S makes the call from that plan. The
 * cases below are the places the plan can put an argument: each integer width,
 * each float width, a register file that runs out, both files at once, and a
 * value type flattened into several of those places at once - straddling the
 * register/stack boundary as an argument, and coming back either scattered
 * across return registers or, when it needs more of those than exist, through
 * a hidden pointer.
 *
 * MONO_LLVM_JIT_DYN_CALL=0 takes every call here back to a gsharedvt_out_sig
 * wrapper, which is the other arm. Both arms must answer OK.
 *
 * Two things are needed to reach the crossing at all, and without either one
 * this file passes while testing nothing:
 *
 * Each callee is marked NoInlining, because the interpreter inlines a small
 * callee into its caller and an inlined body crosses nothing.
 *
 * Each callee is compiled before Main calls it. An interpreted caller settles
 * how it reaches a callee on the first call and keeps that answer, so a callee
 * with no code yet stays interpreted for that caller for good.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

public class DynCall {
	public static int Fail;

	static void Check (string what, long got, long want)
	{
		if (got != want) {
			Console.WriteLine ("FAIL {0}: got {1} want {2}", what, got, want);
			Fail++;
		}
	}

	static void CheckD (string what, double got, double want)
	{
		if (got != want) {
			Console.WriteLine ("FAIL {0}: got {1} want {2}", what, got, want);
			Fail++;
		}
	}

	/* The caller widens what the callee left in the return register, so each
	 * width has to come back with the right sign. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static sbyte RetI1 (int x) { return (sbyte) x; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static byte RetU1 (int x) { return (byte) x; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static short RetI2 (int x) { return (short) x; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static ushort RetU2 (int x) { return (ushort) x; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int RetI4 (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static uint RetU4 (int x) { return (uint) x; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long RetI8 (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static float RetR4 (float x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static double RetR8 (double x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static string RetRef (string x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static bool RetBool (int x) { return x != 0; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static char RetChar (int x) { return (char) x; }

	/* The top of an argument's register slot is this backend's to choose, so
	 * a callee reads only the width it declared. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int TakeNarrow (sbyte a, byte b, short c, ushort d)
	{
		return a + b + c + d;
	}

	/* More integers than the six the file holds. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long TakeTenInts (int a, int b, int c, int d, int e,
	                                int f, int g, int h, int i, int j)
	{
		return a + b + c + d + e + f + g + h + i + j;
	}

	/* More doubles than the eight the file holds. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static double TakeTenDoubles (double a, double b, double c, double d, double e,
	                                     double f, double g, double h, double i, double j)
	{
		return a + b + c + d + e + f + g + h + i + j;
	}

	/* A float that misses the file takes a whole stack word, not four bytes. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static float TakeTenFloats (float a, float b, float c, float d, float e,
	                                   float f, float g, float h, float i, float j)
	{
		return a + b + c + d + e + f + g + h + i + j;
	}

	/* The two files run down independently, so neither fills evenly here. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static double TakeMixed (int a, double b, int c, float d, long e,
	                                double f, int g, float h, int i, double j)
	{
		return a + b + c + d + e + f + g + h + i + j;
	}

	/* An enum is its underlying integer to the convention, and the two below
	 * are different widths. */
	public enum Small : byte { A = 200 }
	public enum Big : long { B = 0x1234567890L }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long TakeEnums (Small a, Big b, int c) { return (long) a + (long) b + c; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Big RetEnum (long x) { return (Big) x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int TakeByref (ref int a, ref long b, out int c)
	{
		c = a + 1;
		b = b + 2;
		return a;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static string TakeObjects (object a, string b, int[] c)
	{
		return String.Concat (a.ToString (), b, c.Length.ToString ());
	}

	/*
	 * A value type argument or return flattens into the leaves LLVM lowers it
	 * into, the same convention an aggregate already crossing the seam
	 * incoming uses. Pair and Mixed have no padding between their fields, so
	 * each one's leaf count is exactly its field count. Quad's four integer
	 * leaves are what push a return past ret_gregs and force a hidden return
	 * pointer instead.
	 */
	[StructLayout (LayoutKind.Sequential)]
	public struct Pair {
		public int A, B;
	}

	[StructLayout (LayoutKind.Sequential)]
	public struct Quad {
		public int A, B, C, D;
	}

	[StructLayout (LayoutKind.Sequential)]
	public struct Mixed {
		public int A, B;
		public double C;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Pair MakePair (int a, int b) { return new Pair { A = a, B = b }; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long TakePair (Pair p, int extra) { return p.A + p.B + extra; }

	/* w, x and y take three of the six integer registers, so Quad's four
	 * leaves fill the rest of the file and spill its last one to the stack. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long TakeQuad (int w, int x, int y, Quad q)
	{
		return w + x + y + q.A + q.B + q.C + q.D;
	}

	/* Four integer leaves are more than ret_gregs holds, so this comes back
	 * through a hidden pointer rather than in registers. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Quad MakeQuad (int a, int b, int c, int d)
	{
		return new Quad { A = a, B = b, C = c, D = d };
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static double TakeMixedStruct (Mixed m) { return m.A + m.B + m.C; }

	/* Two integer leaves and one SSE leaf: within both ret_gregs and
	 * ret_scalar_fregs, so this comes back in registers, one from each
	 * file. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Mixed MakeMixed (int a, int b, double c)
	{
		return new Mixed { A = a, B = b, C = c };
	}

	public int Field;

	/* A receiver is always the first argument. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public long Instance (int a, double b, int c) { return Field + a + (long) b + c; }

	static void CompileCallees ()
	{
		BindingFlags mine = BindingFlags.Public | BindingFlags.Static
		                    | BindingFlags.Instance | BindingFlags.DeclaredOnly;

		foreach (MethodInfo m in typeof (DynCall).GetMethods (mine)) {
			if (!Mono.Tiering.MonoTier.PromoteNow (m.MethodHandle.Value, 1)) {
				Console.WriteLine ("FAIL could not compile {0}", m.Name);
				Fail++;
			}
		}
	}

	public static int Main ()
	{
		CompileCallees ();

		DynCall self = new DynCall ();
		self.Field = 7;

		int[] arr = new int[3];

		for (int n = 0; n < 50; n++) {
			Check ("RetI1", RetI1 (-3), -3);
			Check ("RetI1 trunc", RetI1 (0x80), unchecked ((sbyte) 0x80));
			Check ("RetU1", RetU1 (0x1FF), 0xFF);
			Check ("RetI2", RetI2 (-300), -300);
			Check ("RetU2", RetU2 (0x1FFFF), 0xFFFF);
			Check ("RetI4", RetI4 (-123456), -123456);
			Check ("RetU4", RetU4 (-1), 0xFFFFFFFFL);
			Check ("RetI8", RetI8 (-1234567890123L), -1234567890123L);
			CheckD ("RetR4", RetR4 (1.5f), 1.5);
			CheckD ("RetR8", RetR8 (-2.25), -2.25);
			Check ("RetRef", RetRef ("abc") == "abc" ? 1 : 0, 1);
			Check ("RetBool", RetBool (5) ? 1 : 0, 1);
			Check ("RetChar", RetChar (0x4142), 0x4142);

			Check ("TakeNarrow", TakeNarrow (-1, 255, -300, 65535), -1 + 255 - 300 + 65535);
			Check ("TakeTenInts", TakeTenInts (1, 2, 3, 4, 5, 6, 7, 8, 9, 10), 55);
			CheckD ("TakeTenDoubles", TakeTenDoubles (1, 2, 3, 4, 5, 6, 7, 8, 9, 10), 55);
			CheckD ("TakeTenFloats", TakeTenFloats (1, 2, 3, 4, 5, 6, 7, 8, 9, 10), 55);
			CheckD ("TakeMixed", TakeMixed (1, 2, 3, 4, 5, 6, 7, 8, 9, 10), 55);
			Check ("TakeEnums", TakeEnums (Small.A, Big.B, 3), 200 + 0x1234567890L + 3);
			Check ("RetEnum", (long) RetEnum (0x1234567890L), 0x1234567890L);

			int a = 41;
			long b = 100;
			int c;
			Check ("TakeByref", TakeByref (ref a, ref b, out c), 41);
			Check ("TakeByref out", c, 42);
			Check ("TakeByref ref", b, 102);

			Check ("TakeObjects", TakeObjects (5, "x", arr) == "5x3" ? 1 : 0, 1);
			Check ("Instance", self.Instance (1, 2.0, 3), 13);

			Pair pair = MakePair (11, 22);
			Check ("MakePair.A", pair.A, 11);
			Check ("MakePair.B", pair.B, 22);
			Check ("TakePair", TakePair (pair, 7), 11 + 22 + 7);

			Quad quad = MakeQuad (1, 2, 3, 4);
			Check ("MakeQuad.A", quad.A, 1);
			Check ("MakeQuad.B", quad.B, 2);
			Check ("MakeQuad.C", quad.C, 3);
			Check ("MakeQuad.D", quad.D, 4);
			Check ("TakeQuad", TakeQuad (100, 200, 300, quad), 100 + 200 + 300 + 1 + 2 + 3 + 4);

			Mixed mixed = MakeMixed (5, 6, 7.5);
			Check ("MakeMixed.A", mixed.A, 5);
			Check ("MakeMixed.B", mixed.B, 6);
			CheckD ("MakeMixed.C", mixed.C, 7.5);
			CheckD ("TakeMixedStruct", TakeMixedStruct (mixed), 5 + 6 + 7.5);
		}

		Console.WriteLine (Fail == 0 ? "OK" : "FAILED " + Fail);
		return Fail == 0 ? 0 : 1;
	}
}
