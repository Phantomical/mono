using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * -mono-inline-cost-byte-budget bounds how many IL bytes the tier-2 cost
 * model may still translate into one root, alongside
 * -mono-inline-cost-budget's plain count. A count treats a 5-byte getter the
 * same as a 250-byte body, so it cannot by itself stop two candidates well
 * under the count budget from spending far more translation than a small
 * byte budget allows.
 *
 * CallA () and CallB () are each 113 IL bytes -- both loop-free, so
 * tier2_site_heat () answers both their sites hot, and the shipped
 * -mono-inline-cost-il-limit-hot default (1024) admits either alone with
 * plenty to spare. What decides whether both fold is the byte budget alone.
 * The suite runs twice: on a small byte budget (150, past the 113 either
 * spends alone but under the 226 both together spend) and on a huge one that
 * is not the binding constraint. The count budget stays at its default in
 * both, comfortably above 2, so it never explains a difference between the
 * arms.
 *
 * Which of the two candidates the byte budget catches is not fixed --
 * TopDownInlinerPass ranks equally-hot sites in whatever order ties break in,
 * and this file does not depend on which one wins. What it checks is the
 * count that folds: both under the huge budget, and strictly fewer than both
 * under the small one.
 *
 * What says a fold happened is the stack trace, the way
 * tier2-inline-cost.cs reads it: a folded body owns no code, so its frame
 * reports the offset into Root () it was folded at, and a body that was
 * really called reports an offset into itself.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Costed {
	public static int CallA (int n, bool yes)
	{
		int a = n + 1, b = n + 2, c = n + 3, d = n + 4;

		a = a * b + c - d;
		b = b * c + d - a;
		c = c * d + a - b;
		d = d * a + b - c;
		a = a ^ b ^ c ^ d;
		b = b | c | d | a;
		c = c & d & a & b;
		d = d + a + b + c;

		if (yes)
			throw new InvalidOperationException ("a");

		return a + b + c + d;
	}

	// The same shape as CallA () under a name of its own, so each site's fold
	// decision is independent -- materialize () hands back a standing copy
	// once a callee is folded anywhere in the root, which would fold this one
	// for free off CallA ()'s copy if they shared a name.
	public static int CallB (int n, bool yes)
	{
		int a = n + 1, b = n + 2, c = n + 3, d = n + 4;

		a = a * b + c - d;
		b = b * c + d - a;
		c = c * d + a - b;
		d = d * a + b - c;
		a = a ^ b ^ c ^ d;
		b = b | c | d | a;
		c = c & d & a & b;
		d = d + a + b + c;

		if (yes)
			throw new InvalidOperationException ("b");

		return a + b + c + d;
	}
}

static class Program {
	static bool sawA, sawB, foldedA, foldedB;

	static bool RunsInsideRoot (Exception e, string helper)
	{
		StackTrace st = new StackTrace (e, false);
		int in_helper = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Costed" && m.Name == helper)
				in_helper = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_helper >= 0 && in_helper == in_root;
	}

	static int Root (int n, bool takeRare)
	{
		int total = 0;

		try {
			total += Costed.CallA (n, takeRare);
		} catch (InvalidOperationException e) {
			sawA = true;
			foldedA = RunsInsideRoot (e, "CallA");
		}

		try {
			total += Costed.CallB (n, takeRare);
		} catch (InvalidOperationException e) {
			sawB = true;
			foldedB = RunsInsideRoot (e, "CallB");
		}

		return total;
	}

	static int fails;

	static void Check (bool ok, string what)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	public static int Main ()
	{
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		sawA = sawB = foldedA = foldedB = false;

		Root (4, true);

		Check (sawA && sawB, "both call sites threw at tier 2");

		bool small = Environment.GetEnvironmentVariable ("MONO_INLINE_POLICY") != "off";
		int foldedCount = (foldedA ? 1 : 0) + (foldedB ? 1 : 0);

		if (small)
			Check (foldedCount == 1,
				"150 admits one of the two 113-byte candidates and leaves too "
				+ "little for the other, however the count budget reads them");
		else
			Check (foldedCount == 2,
				"a byte budget that is not the binding constraint folds both, "
				+ "same as the count budget alone would");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
