using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Two limits on the shape-test pre-pass, on top of -mono-inline-budget's
 * count of distinct bodies: -mono-inline-trivial-fanout-limit refuses a
 * single callee whose own call sites in one root outnumber it, and
 * -mono-inline-trivial-instance-budget refuses a callee once building it a
 * fresh copy would spend more sites than the root has left, summed across
 * every callee already built one. Neither is the other: a callee under the
 * fanout limit can still be the one the instance budget catches, and a
 * callee the fanout limit refuses spends nothing towards that budget.
 *
 * Both count static call sites, so Root () below writes each one out rather
 * than calling from inside a loop, which would compile to one site run
 * several times rather than several sites.
 *
 * Both are pre-pass limits, not the cost model's, so -mono-inline-il-limit
 * has to stay on for either to have a candidate to look at, and
 * MonoTier::PromoteNow asks for tier 1 explicitly, for the reason
 * trivial-inline.cs gives.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Helpers {
	// 3 sites in Root (): under both limits, so this inlines.
	public static void Small (int x) { throw new InvalidOperationException ("small"); }

	// 8 sites in Root (): over the fanout limit on its own, whatever the
	// instance budget still has.
	public static void Wide (int x) { throw new InvalidOperationException ("wide"); }

	// 4 sites in Root (), read after Small () has already spent 3 of the
	// instance budget: 3 + 4 is still within it, so this inlines too.
	public static void Extra (int x) { throw new InvalidOperationException ("extra"); }

	// 4 sites in Root (), read after Small () and Extra () between them have
	// spent 7: 7 + 4 is past the budget, so this is left uncalled by the
	// pre-pass even though 4 sites is under the fanout limit alone.
	public static void TooMuch (int x) { throw new InvalidOperationException ("too much"); }
}

static class Program {
	/* Which of Helpers' bodies ran inside Root ()'s own code. */
	static bool RunsInside (Exception e, string helper, string root)
	{
		StackTrace st = new StackTrace (e, false);
		int in_helper = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Helpers" && m.Name == helper)
				in_helper = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == root)
				in_root = f.GetNativeOffset ();
		}

		return in_helper >= 0 && in_helper == in_root;
	}

	static bool inlined_small, inlined_wide, inlined_extra, inlined_too_much;

	static void Root (int n)
	{
		try {
			Helpers.Small (n + 0);
		} catch (InvalidOperationException e) {
			inlined_small |= RunsInside (e, "Small", "Root");
		}
		try {
			Helpers.Small (n + 1);
		} catch (InvalidOperationException e) {
			inlined_small |= RunsInside (e, "Small", "Root");
		}
		try {
			Helpers.Small (n + 2);
		} catch (InvalidOperationException e) {
			inlined_small |= RunsInside (e, "Small", "Root");
		}

		try {
			Helpers.Wide (n + 0);
		} catch (InvalidOperationException e) {
			inlined_wide |= RunsInside (e, "Wide", "Root");
		}
		try {
			Helpers.Wide (n + 1);
		} catch (InvalidOperationException e) {
			inlined_wide |= RunsInside (e, "Wide", "Root");
		}
		try {
			Helpers.Wide (n + 2);
		} catch (InvalidOperationException e) {
			inlined_wide |= RunsInside (e, "Wide", "Root");
		}
		try {
			Helpers.Wide (n + 3);
		} catch (InvalidOperationException e) {
			inlined_wide |= RunsInside (e, "Wide", "Root");
		}
		try {
			Helpers.Wide (n + 4);
		} catch (InvalidOperationException e) {
			inlined_wide |= RunsInside (e, "Wide", "Root");
		}
		try {
			Helpers.Wide (n + 5);
		} catch (InvalidOperationException e) {
			inlined_wide |= RunsInside (e, "Wide", "Root");
		}
		try {
			Helpers.Wide (n + 6);
		} catch (InvalidOperationException e) {
			inlined_wide |= RunsInside (e, "Wide", "Root");
		}
		try {
			Helpers.Wide (n + 7);
		} catch (InvalidOperationException e) {
			inlined_wide |= RunsInside (e, "Wide", "Root");
		}

		try {
			Helpers.Extra (n + 0);
		} catch (InvalidOperationException e) {
			inlined_extra |= RunsInside (e, "Extra", "Root");
		}
		try {
			Helpers.Extra (n + 1);
		} catch (InvalidOperationException e) {
			inlined_extra |= RunsInside (e, "Extra", "Root");
		}
		try {
			Helpers.Extra (n + 2);
		} catch (InvalidOperationException e) {
			inlined_extra |= RunsInside (e, "Extra", "Root");
		}
		try {
			Helpers.Extra (n + 3);
		} catch (InvalidOperationException e) {
			inlined_extra |= RunsInside (e, "Extra", "Root");
		}

		try {
			Helpers.TooMuch (n + 0);
		} catch (InvalidOperationException e) {
			inlined_too_much |= RunsInside (e, "TooMuch", "Root");
		}
		try {
			Helpers.TooMuch (n + 1);
		} catch (InvalidOperationException e) {
			inlined_too_much |= RunsInside (e, "TooMuch", "Root");
		}
		try {
			Helpers.TooMuch (n + 2);
		} catch (InvalidOperationException e) {
			inlined_too_much |= RunsInside (e, "TooMuch", "Root");
		}
		try {
			Helpers.TooMuch (n + 3);
		} catch (InvalidOperationException e) {
			inlined_too_much |= RunsInside (e, "TooMuch", "Root");
		}
	}

	static int fails;

	static void Check (bool ok, string what)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	/* MonoTier::tier1, as PromoteNow takes it. */
	const int tier1 = 2;

	public static int Main ()
	{
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, tier1)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		Root (1);

		Check (inlined_small, "a callee under both limits inlines");
		Check (!inlined_wide, "a callee over the fanout limit alone stays uncalled");
		Check (inlined_extra,
		       "a callee the instance budget still has room for inlines too");
		Check (!inlined_too_much,
		       "a callee under the fanout limit is still refused once the "
		       + "instance budget it would spend is not left");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
