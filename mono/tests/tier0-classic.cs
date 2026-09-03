using System;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

// --llvm-opt=-mono-tier0-classic=ClassicExercise filters two methods to the
// classic compiler: Tier0ClassicExercise and the callee it calls,
// Tier0ClassicExerciseCallee. Every other method here still interprets. Run
// unfiltered, everything interprets too, and both arms have to reach the same
// array of checks.
//
// Calls out of a classic body now resolve through each callee's own thunk,
// the address every other engine's calls resolve to. That lets
// Tier0ClassicExercise allocate its own arrays and objects with
// newarr/newobj, and call another classic-compiled method, instead of taking
// them all in as arguments.
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

	// Matched by the same filter substring as Tier0ClassicExercise, and called
	// only from inside it. This is a classic body calling another classic
	// body, not one calling out to an interpreted or LLVM-compiled one.
	static int Tier0ClassicExerciseCallee (int x)
	{
		return x * 3 + 11;
	}

	public static long[] Tier0ClassicExercise ()
	{
		long[] r = new long[Names.Length];
		int[] arr = new int[6];
		int[] tiny = new int[1];
		Shape shape = new Square ();

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
		r[i++] = arr.Length;

		int called = AddOne (41);
		int sides = shape.Sides ();
		r[i++] = called;
		r[i++] = sides;
		r[i++] = Tier0ClassicExerciseCallee (9);

		string s = "n=" + called.ToString () + ",sides=" + sides.ToString ();
		r[i++] = s.Length;

		int caught = 0;
		try {
			caught = tiny[2];
		} catch (IndexOutOfRangeException) {
			caught = 99;
		}
		r[i++] = caught;

		return r;
	}

	static readonly string[] Names = {
		"int add", "int sub", "int mul", "int div signed", "int rem signed",
		"uint div", "uint rem",
		"long div pos", "long rem pos", "long div neg", "long rem neg",
		"ulong div", "ulong rem",
		"float chain *1e6", "double calc *1e6",
		"float<-long conv", "double<-int conv",
		"loop+switch acc", "array sum", "array alloc length",
		"call", "virtual call", "classic callee", "string length", "catch",
	};

	static readonly long[] Want = {
		12, 22, -85, -3, 2,
		14, 2,
		333333333, 1, -333333333, 1,
		3333333333, 1,
		601562, 2750000,
		1000000000, 17,
		14, 55, 6,
		42, 4, 38, 12, 99,
	};

	static int Check (long[] r)
	{
		int failures = 0;

		for (int i = 0; i < Want.Length; i++) {
			if (r [i] != Want [i]) {
				Console.WriteLine ("FAIL: {0} = {1}, want {2}", Names [i], r [i], Want [i]);
				failures++;
			}
		}

		return failures;
	}

	public static int Main ()
	{
		MethodInfo exercise = typeof (Tier0ClassicTest).GetMethod ("Tier0ClassicExercise");
		MethodInfo callee = typeof (Tier0ClassicTest).GetMethod ("Tier0ClassicExerciseCallee",
			BindingFlags.Static | BindingFlags.NonPublic);

		// A direct call from Main, itself interpreted, never asks the backend
		// for its callee - the interpreter resolves and runs one on its own.
		// Reflection crosses through a compiled runtime-invoke wrapper instead,
		// which is what gives Tier0ClassicExercise's own entry a chance to
		// answer the filter.
		int failures = Check ((long[]) exercise.Invoke (null, null));

		// Tier0ClassicExercise is already compiled, with the callee's thunk
		// address baked into its own call site. Move the callee off tier 0 and
		// onto tier 1 without disturbing that caller. Calling the caller again
		// makes the same call site reach a different body through the same
		// thunk.
		const int tier1 = 3;
		if (!Mono.Tiering.MonoTier.PromoteNow (callee.MethodHandle.Value, tier1)) {
			Console.WriteLine ("FAIL: Tier0ClassicExerciseCallee () would not promote to tier 1");
			failures++;
		}

		failures += Check ((long[]) exercise.Invoke (null, null));

		if (failures != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
