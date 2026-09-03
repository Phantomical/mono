using System;
using System.Reflection;

// --llvm-opt=-mono-tier0-classic=Tier0ClassicExercise filters that one method
// to the classic compiler. Every other method here still interprets. Run
// unfiltered, it interprets too, and both arms have to reach the same array
// of checks.
//
// Every buffer Tier0ClassicExercise fills in is allocated by its caller and
// passed in, rather than with newobj/newarr inside the method. A
// classic-compiled body's own direct call to the allocator wrapper goes
// through mono_create_jit_trampoline (), which does not register the
// wrapper's MonoJitInfo, and mini_jit_info_table_find () then aborts the
// process. So the allocations stay outside the filtered method until a
// classic body's calls resolve through each callee's thunk.
public class Tier0ClassicTest
{
	public abstract class Shape
	{
		public abstract int Sides ();
	}

	public sealed class Square : Shape
	{
		public override int Sides () { return 4; }
	}

	static int AddOne (int x)
	{
		return x + 1;
	}

	static float RoundTripFloat (float v)
	{
		return v;
	}

	public static void Tier0ClassicExercise (long[] r, int[] arr, int[] tiny, Shape shape)
	{
		int i = 0;

		int ia = 17, ib = -5;
		r[i++] = ia + ib;
		r[i++] = ia - ib;
		r[i++] = ia * ib;
		r[i++] = ia / ib;
		r[i++] = ia % ib;

		uint ua = 100u, ub = 7u;
		r[i++] = ua / ub;
		r[i++] = ua % ub;

		long la = 1000000000L, lb = 3L, lc = -3L;
		r[i++] = la / lb;
		r[i++] = la % lb;
		r[i++] = la / lc;
		r[i++] = la % lc;

		ulong ula = 10000000000UL, ulb = 3UL;
		r[i++] = (long) (ula / ulb);
		r[i++] = (long) (ula % ulb);

		// Adding three small increments to a large value and subtracting it
		// back out rounds differently under 32-bit float than under double.
		// RoundTripFloat sends every intermediate through an argument and a
		// return value, so a widened R4 shows up here.
		float f = 100000f;
		f = RoundTripFloat (f + 0.1f);
		f = RoundTripFloat (f + 0.2f);
		f = RoundTripFloat (f + 0.3f);
		f = RoundTripFloat (f - 100000f);
		r[i++] = (long) (f * 1000000f);

		double d = 10.0 / 4.0 + 0.25;
		r[i++] = (long) (d * 1000000.0);

		float fromLong = (float) la;
		r[i++] = (long) fromLong;

		double fromInt = (double) ia;
		r[i++] = (int) fromInt;

		long acc = 0;
		for (int k = 0; k < 10; k++) {
			if ((k & 1) == 0)
				acc += k;
			else
				acc -= k;

			switch (k % 3) {
			case 0: acc += 1; break;
			case 1: acc += 2; break;
			default: acc += 3; break;
			}
		}
		r[i++] = acc;

		for (int k = 0; k < arr.Length; k++)
			arr[k] = k * k;
		long arrSum = 0;
		for (int k = 0; k < arr.Length; k++)
			arrSum += arr[k];
		r[i++] = arrSum;

		int called = AddOne (41);
		int sides = shape.Sides ();
		r[i++] = called;
		r[i++] = sides;

		string s = "n=" + called.ToString () + ",sides=" + sides.ToString ();
		r[i++] = s.Length;

		int caught = 0;
		try {
			caught = tiny[2];
		} catch (IndexOutOfRangeException) {
			caught = 99;
		}
		r[i++] = caught;
	}

	static readonly string[] Names = {
		"int add", "int sub", "int mul", "int div signed", "int rem signed",
		"uint div", "uint rem",
		"long div pos", "long rem pos", "long div neg", "long rem neg",
		"ulong div", "ulong rem",
		"float chain *1e6", "double calc *1e6",
		"float<-long conv", "double<-int conv",
		"loop+switch acc", "array sum",
		"call", "virtual call", "string length", "catch",
	};

	static readonly long[] Want = {
		12, 22, -85, -3, 2,
		14, 2,
		333333333, 1, -333333333, 1,
		3333333333, 1,
		601562, 2750000,
		1000000000, 17,
		14, 55,
		42, 4, 12, 99,
	};

	public static int Main ()
	{
		long[] r = new long[Names.Length];
		int[] arr = new int[6];
		int[] tiny = new int[1];
		Shape shape = new Square ();

		// A direct call from Main, itself interpreted, never asks the backend
		// for its callee - the interpreter resolves and runs one on its own.
		// Reflection crosses through a compiled runtime-invoke wrapper instead,
		// which is what gives Tier0ClassicExercise's own entry a chance to
		// answer the filter.
		MethodInfo m = typeof (Tier0ClassicTest).GetMethod ("Tier0ClassicExercise");
		m.Invoke (null, new object[] { r, arr, tiny, shape });

		int failures = 0;

		for (int i = 0; i < Want.Length; i++) {
			if (r [i] != Want [i]) {
				Console.WriteLine ("FAIL: {0} = {1}, want {2}", Names [i], r [i], Want [i]);
				failures++;
			}
		}

		if (failures != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
