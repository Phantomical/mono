using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * The System.Math and System.MathF icalls the compiled tiers answer with
 * arithmetic instead of a call to the icall's wrapper.
 *
 * Two things are checked, and they catch different faults.
 *
 * Sample () computes every operation and the tiers are compared entry by entry.
 * The interpreter answers each one with a MINT opcode over the same libm
 * function the icall calls, and the compiled tiers with an intrinsic, so the
 * three have to agree bit for bit. A tier that answers differently is what an
 * argument in the wrong order or a wrong overload looks like.
 *
 * CheckPinned () holds the answers the standard settles, which no amount of
 * agreement between the tiers can supply. Math.Round is the reason it is worth
 * writing down separately: llvm.round and llvm.roundeven differ on a half, and
 * both agree with the icall everywhere else.
 *
 * Most arguments come out of static fields, because a literal lets a compiled
 * tier fold the call at compile time and this test is about the instruction
 * each tier emits. SampleFolded () is the other half, with literals, so the
 * constant folder answers alongside libm and the interpreter.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Program {
	static double dv;
	static float fv;

	static double D (double x) { dv = x; return dv; }
	static float F (float x) { fv = x; return fv; }

	static int fails;

	static void Fail (string what, double got, string want)
	{
		Console.WriteLine ("FAIL: {0} gave {1:R} ({2:x16}), wanted {3}",
			what, got, BitConverter.DoubleToInt64Bits (got), want);
		++fails;
	}

	/* Bit for bit, so that the two zeros stay apart. */
	static void Same (string what, double got, double want)
	{
		if (BitConverter.DoubleToInt64Bits (got) == BitConverter.DoubleToInt64Bits (want))
			return;

		Fail (what, got, want.ToString ("R"));
	}

	static void Same (string what, float got, float want)
	{
		Same (what, (double) got, (double) want);
	}

	/* The sign and the payload of a NaN are the operation's own business. */
	static void NaN (string what, double got)
	{
		if (double.IsNaN (got))
			return;

		Fail (what, got, "a NaN");
	}

	static void NaN (string what, float got)
	{
		NaN (what, (double) got);
	}

	static readonly double[] arguments = {
		double.NaN, double.PositiveInfinity, double.NegativeInfinity,
		0.0, -0.0, 1.0, -1.0, 2.0, -2.0, 0.5, -0.5, 1.5, 2.5, 4.0, 8.0, -8.0,
		100.0, 2.7, -2.7, 0.49999999999999994,
	};

	static readonly double[] exponents = {
		0.0, -0.0, 1.0, -1.0, 2.0, 10.0,
		double.NaN, double.PositiveInfinity, double.NegativeInfinity,
	};

	/*
	 * The same operations over literal arguments. A compiled tier folds these
	 * at compile time, in LLVM's constant folder rather than in libm, and the
	 * interpreter still computes them. Pow is where the two can part company:
	 * libm answers 1 for a NaN base with a zero exponent, and the .NET
	 * documentation says NaN.
	 */
	static void SampleFolded (List<double> got)
	{
		got.Add (Math.Sqrt (4.0));
		got.Add (Math.Sqrt (-1.0));
		got.Add (Math.Sqrt (-0.0));
		got.Add (Math.Abs (-0.0));
		got.Add (Math.Floor (-2.7));
		got.Add (Math.Ceiling (-0.5));
		got.Add (Math.Exp (0.0));
		got.Add (Math.Log (0.0));
		got.Add (Math.Log10 (100.0));
		got.Add (Math.Cbrt (-8.0));
		got.Add (Math.Sin (0.0));
		got.Add (Math.Tanh (double.PositiveInfinity));
		got.Add (Math.Pow (0.0, 0.0));
		got.Add (Math.Pow (double.NaN, 0.0));
		got.Add (Math.Pow (1.0, double.NaN));
		got.Add (Math.Pow (-1.0, double.PositiveInfinity));
		got.Add (Math.Pow (0.0, -1.0));
		got.Add (Math.Pow (2.0, 10.0));
		got.Add (Math.Atan2 (0.0, -1.0));
		got.Add (Math.Atan2 (-0.0, -1.0));
		got.Add (MathF.Sqrt (4.0f));
		got.Add (MathF.Sqrt (-1.0f));
		got.Add (MathF.Abs (-0.0f));
		got.Add (MathF.Cbrt (-8.0f));
		got.Add (MathF.Pow (2.0f, 10.0f));
		got.Add (MathF.Pow (float.NaN, 0.0f));
	}

	static double[] Sample ()
	{
		List<double> got = new List<double> ();

		SampleFolded (got);

		foreach (double x in arguments) {
			got.Add (Math.Abs (D (x)));
			got.Add (Math.Sqrt (D (x)));
			got.Add (Math.Sin (D (x)));
			got.Add (Math.Cos (D (x)));
			got.Add (Math.Tan (D (x)));
			got.Add (Math.Asin (D (x)));
			got.Add (Math.Acos (D (x)));
			got.Add (Math.Atan (D (x)));
			got.Add (Math.Sinh (D (x)));
			got.Add (Math.Cosh (D (x)));
			got.Add (Math.Tanh (D (x)));
			got.Add (Math.Asinh (D (x)));
			got.Add (Math.Acosh (D (x)));
			got.Add (Math.Atanh (D (x)));
			got.Add (Math.Exp (D (x)));
			got.Add (Math.Log (D (x)));
			got.Add (Math.Log10 (D (x)));
			got.Add (Math.Floor (D (x)));
			got.Add (Math.Ceiling (D (x)));
			got.Add (Math.Round (D (x)));
			got.Add (Math.Cbrt (D (x)));
			// The Abs(single) icall, which lives on Math rather than on MathF.
			got.Add (Math.Abs (F ((float) x)));

			float f = (float) x;

			got.Add (MathF.Abs (F (f)));
			got.Add (MathF.Sqrt (F (f)));
			got.Add (MathF.Sin (F (f)));
			got.Add (MathF.Cos (F (f)));
			got.Add (MathF.Tan (F (f)));
			got.Add (MathF.Atan (F (f)));
			got.Add (MathF.Tanh (F (f)));
			got.Add (MathF.Exp (F (f)));
			got.Add (MathF.Log (F (f)));
			got.Add (MathF.Cbrt (F (f)));
			got.Add (MathF.Asinh (F (f)));
			got.Add (MathF.Acosh (F (f)));
			got.Add (MathF.Atanh (F (f)));
		}

		foreach (double y in exponents) {
			got.Add (Math.Pow (D (2.5), D (y)));
			got.Add (Math.Pow (D (y), D (2.5)));
			got.Add (Math.Atan2 (D (2.5), D (y)));
			got.Add (Math.Atan2 (D (y), D (2.5)));
			// IEEERemainder is managed IL over the FMod icall.
			got.Add (Math.IEEERemainder (D (2.5), D (y)));
			got.Add (MathF.Pow (F (2.5f), F ((float) y)));
			got.Add (MathF.Atan2 (F (2.5f), F ((float) y)));
			got.Add (MathF.IEEERemainder (F (2.5f), F ((float) y)));
		}

		return got.ToArray ();
	}

	static void CheckPinned ()
	{
		Same ("Math.Sqrt (4)", Math.Sqrt (D (4.0)), 2.0);
		Same ("Math.Sqrt (0)", Math.Sqrt (D (0.0)), 0.0);
		Same ("Math.Sqrt (-0)", Math.Sqrt (D (-0.0)), -0.0);
		Same ("Math.Sqrt (Infinity)", Math.Sqrt (D (double.PositiveInfinity)),
			double.PositiveInfinity);
		NaN ("Math.Sqrt (-1)", Math.Sqrt (D (-1.0)));
		NaN ("Math.Sqrt (NaN)", Math.Sqrt (D (double.NaN)));

		Same ("Math.Abs (-2.5)", Math.Abs (D (-2.5)), 2.5);
		Same ("Math.Abs (-0)", Math.Abs (D (-0.0)), 0.0);
		Same ("Math.Abs (-Infinity)", Math.Abs (D (double.NegativeInfinity)),
			double.PositiveInfinity);
		NaN ("Math.Abs (NaN)", Math.Abs (D (double.NaN)));

		Same ("Math.Floor (2.7)", Math.Floor (D (2.7)), 2.0);
		Same ("Math.Floor (-2.7)", Math.Floor (D (-2.7)), -3.0);
		Same ("Math.Floor (-0.5)", Math.Floor (D (-0.5)), -1.0);
		Same ("Math.Floor (-0)", Math.Floor (D (-0.0)), -0.0);
		Same ("Math.Ceiling (2.7)", Math.Ceiling (D (2.7)), 3.0);
		Same ("Math.Ceiling (-2.7)", Math.Ceiling (D (-2.7)), -2.0);
		Same ("Math.Ceiling (-0.5)", Math.Ceiling (D (-0.5)), -0.0);
		Same ("Math.Ceiling (0)", Math.Ceiling (D (0.0)), 0.0);

		/*
		 * Round is the icall, in both engines. mono_round_to_even () rounds a
		 * half to even the way llvm.roundeven does, and llvm.round does not, so
		 * the three halves below would catch that substitution. The last case
		 * is where the icall and the intrinsic part company: the icall computes
		 * floor (x + 0.5), and the addition carries the largest double under a
		 * half up to one. Answering it with the intrinsic would make Math.Round
		 * depend on which tier the caller runs at.
		 */
		Same ("Math.Round (0.5)", Math.Round (D (0.5)), 0.0);
		Same ("Math.Round (1.5)", Math.Round (D (1.5)), 2.0);
		Same ("Math.Round (2.5)", Math.Round (D (2.5)), 2.0);
		Same ("Math.Round (-0.5)", Math.Round (D (-0.5)), -0.0);
		Same ("Math.Round (-1.5)", Math.Round (D (-1.5)), -2.0);
		Same ("Math.Round (0.49999999999999994)",
			Math.Round (D (0.49999999999999994)), 1.0);

		Same ("Math.Exp (0)", Math.Exp (D (0.0)), 1.0);
		Same ("Math.Exp (-Infinity)", Math.Exp (D (double.NegativeInfinity)), 0.0);
		Same ("Math.Exp (Infinity)", Math.Exp (D (double.PositiveInfinity)),
			double.PositiveInfinity);
		Same ("Math.Log (1)", Math.Log (D (1.0)), 0.0);
		Same ("Math.Log (0)", Math.Log (D (0.0)), double.NegativeInfinity);
		NaN ("Math.Log (-1)", Math.Log (D (-1.0)));
		Same ("Math.Log10 (100)", Math.Log10 (D (100.0)), 2.0);
		Same ("Math.Log10 (0)", Math.Log10 (D (0.0)), double.NegativeInfinity);

		Same ("Math.Sin (0)", Math.Sin (D (0.0)), 0.0);
		Same ("Math.Cos (0)", Math.Cos (D (0.0)), 1.0);
		Same ("Math.Tan (0)", Math.Tan (D (0.0)), 0.0);
		Same ("Math.Asin (0)", Math.Asin (D (0.0)), 0.0);
		Same ("Math.Acos (1)", Math.Acos (D (1.0)), 0.0);
		Same ("Math.Atan (0)", Math.Atan (D (0.0)), 0.0);
		NaN ("Math.Sin (Infinity)", Math.Sin (D (double.PositiveInfinity)));
		NaN ("Math.Asin (2)", Math.Asin (D (2.0)));
		NaN ("Math.Acos (2)", Math.Acos (D (2.0)));

		Same ("Math.Sinh (0)", Math.Sinh (D (0.0)), 0.0);
		Same ("Math.Sinh (-Infinity)", Math.Sinh (D (double.NegativeInfinity)),
			double.NegativeInfinity);
		Same ("Math.Cosh (0)", Math.Cosh (D (0.0)), 1.0);
		Same ("Math.Cosh (-Infinity)", Math.Cosh (D (double.NegativeInfinity)),
			double.PositiveInfinity);
		Same ("Math.Tanh (0)", Math.Tanh (D (0.0)), 0.0);
		Same ("Math.Tanh (Infinity)", Math.Tanh (D (double.PositiveInfinity)), 1.0);

		// The inverse hyperbolic functions, whose libm symbols
		// runtime/builtins.cpp registers by hand.
		Same ("Math.Asinh (0)", Math.Asinh (D (0.0)), 0.0);
		Same ("Math.Asinh (-0)", Math.Asinh (D (-0.0)), -0.0);
		Same ("Math.Acosh (1)", Math.Acosh (D (1.0)), 0.0);
		NaN ("Math.Acosh (0)", Math.Acosh (D (0.0)));
		Same ("Math.Atanh (0)", Math.Atanh (D (0.0)), 0.0);
		Same ("Math.Atanh (1)", Math.Atanh (D (1.0)), double.PositiveInfinity);
		Same ("Math.Atanh (-1)", Math.Atanh (D (-1.0)), double.NegativeInfinity);
		NaN ("Math.Atanh (2)", Math.Atanh (D (2.0)));
		Same ("MathF.Asinh (0)", MathF.Asinh (F (0.0f)), 0.0f);
		Same ("MathF.Acosh (1)", MathF.Acosh (F (1.0f)), 0.0f);
		Same ("MathF.Atanh (1)", MathF.Atanh (F (1.0f)), float.PositiveInfinity);

		Same ("Math.Cbrt (8)", Math.Cbrt (D (8.0)), 2.0);
		Same ("Math.Cbrt (-8)", Math.Cbrt (D (-8.0)), -2.0);
		Same ("Math.Cbrt (-0)", Math.Cbrt (D (-0.0)), -0.0);
		NaN ("Math.Cbrt (NaN)", Math.Cbrt (D (double.NaN)));

		/*
		 * The libm answers, which are not the ones the .NET documentation
		 * gives. Both engines call the same function, and a constant fold in
		 * the optimizer has to reach them too.
		 */
		Same ("Math.Pow (2, 10)", Math.Pow (D (2.0), D (10.0)), 1024.0);
		Same ("Math.Pow (0, 0)", Math.Pow (D (0.0), D (0.0)), 1.0);
		Same ("Math.Pow (NaN, 0)", Math.Pow (D (double.NaN), D (0.0)), 1.0);
		Same ("Math.Pow (1, NaN)", Math.Pow (D (1.0), D (double.NaN)), 1.0);
		Same ("Math.Pow (-1, Infinity)",
			Math.Pow (D (-1.0), D (double.PositiveInfinity)), 1.0);
		Same ("Math.Pow (0, -1)", Math.Pow (D (0.0), D (-1.0)),
			double.PositiveInfinity);
		Same ("Math.Pow (-0, -1)", Math.Pow (D (-0.0), D (-1.0)),
			double.NegativeInfinity);

		Same ("Math.Atan2 (0, -1)", Math.Atan2 (D (0.0), D (-1.0)), Math.PI);
		Same ("Math.Atan2 (-0, -1)", Math.Atan2 (D (-0.0), D (-1.0)), -Math.PI);
		NaN ("Math.Atan2 (NaN, 1)", Math.Atan2 (D (double.NaN), D (1.0)));

		// The same set over the single-precision icalls, where the argument and
		// the result are float rather than double.
		Same ("MathF.Sqrt (4)", MathF.Sqrt (F (4.0f)), 2.0f);
		NaN ("MathF.Sqrt (-1)", MathF.Sqrt (F (-1.0f)));
		Same ("MathF.Abs (-0)", MathF.Abs (F (-0.0f)), 0.0f);
		Same ("Math.Abs (-0f)", Math.Abs (F (-0.0f)), 0.0f);
		Same ("MathF.Floor (-2.5)", MathF.Floor (F (-2.5f)), -3.0f);
		Same ("MathF.Ceiling (2.5)", MathF.Ceiling (F (2.5f)), 3.0f);
		Same ("MathF.Exp (0)", MathF.Exp (F (0.0f)), 1.0f);
		Same ("MathF.Log (1)", MathF.Log (F (1.0f)), 0.0f);
		Same ("MathF.Log10 (100)", MathF.Log10 (F (100.0f)), 2.0f);
		Same ("MathF.Cos (0)", MathF.Cos (F (0.0f)), 1.0f);
		Same ("MathF.Tanh (Infinity)", MathF.Tanh (F (float.PositiveInfinity)), 1.0f);
		Same ("MathF.Cbrt (8)", MathF.Cbrt (F (8.0f)), 2.0f);
		Same ("MathF.Cbrt (-8)", MathF.Cbrt (F (-8.0f)), -2.0f);
		Same ("MathF.Pow (2, 10)", MathF.Pow (F (2.0f), F (10.0f)), 1024.0f);
		Same ("MathF.Pow (NaN, 0)", MathF.Pow (F (float.NaN), F (0.0f)), 1.0f);
		NaN ("MathF.Atan2 (NaN, 1)", MathF.Atan2 (F (float.NaN), F (1.0f)));
	}

	static void CompareWith (string tier, double[] want, double[] got)
	{
		for (int i = 0; i < want.Length; i++) {
			long a = BitConverter.DoubleToInt64Bits (want[i]);
			long b = BitConverter.DoubleToInt64Bits (got[i]);

			if (a == b)
				continue;
			// A NaN keeps whichever payload the operation gave it.
			if (double.IsNaN (want[i]) && double.IsNaN (got[i]))
				continue;

			Console.WriteLine ("FAIL: sample {0} at {1} is {2:x16}, was {3:x16}",
				i, tier, b, a);
			++fails;
		}
	}

	static bool Promote (MethodInfo method, int tier, string what)
	{
		if (Mono.Tiering.MonoTier.PromoteNow (method.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: {0} would not compile at tier {1}", what, tier);
		++fails;
		return false;
	}

	public static int Main ()
	{
		MethodInfo sample = typeof (Program).GetMethod ("Sample",
			BindingFlags.Static | BindingFlags.NonPublic);
		MethodInfo pinned = typeof (Program).GetMethod ("CheckPinned",
			BindingFlags.Static | BindingFlags.NonPublic);

		double[] first = Sample ();

		CheckPinned ();

		/*
		 * Asked for rather than waited for. An interpreted caller reaches an
		 * interpreted callee without the runtime being asked for it, so a loop
		 * alone leaves both methods where they started.
		 */
		if (!Promote (sample, 2, "Sample ()") || !Promote (pinned, 2, "CheckPinned ()"))
			return 1;

		CompareWith ("tier 1", first, Sample ());
		CheckPinned ();

		// Counts for the tier-2 compile to lay the bodies out against.
		for (int i = 0; i < 200; i++)
			Sample ();

		if (!Promote (sample, 3, "Sample ()") || !Promote (pinned, 3, "CheckPinned ()"))
			return 1;

		CompareWith ("tier 2", first, Sample ());
		CheckPinned ();

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
