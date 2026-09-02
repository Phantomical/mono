using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * -mono-inline-cost-il-limit-hot and -cold let the tier-2 cost model translate
 * a bigger candidate at a hot site than at a cold one, instead of the one flat
 * -mono-inline-cost-il-limit every site shares by default.
 *
 * The suite sets the flat limit to 0, which refuses every site tier2_site_heat
 * () does not answer hot or cold, and hot/cold to 500/100 -- so the two knobs
 * below are the only thing that can fold anything here. HotCall () and
 * ColdCall () are the same body under two names: materialize () hands back a
 * standing copy once a callee is folded anywhere in the root, which would
 * fold the cold site for free off the hot site's copy under one shared name
 * and defeat the test. Both bodies are tier2-inline-cost.cs's FailLong (),
 * which that file documents as just past the *flat default* of 256 -- clearly
 * under -hot's 500 here and clearly over -cold's 100.
 *
 * What says a fold happened is the stack trace, the way tier2-inline-cost.cs
 * reads it: a folded body owns no code, so its frame reports the offset into
 * Root () it was folded at, and a body that was really called reports an
 * offset into itself.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Costed {
	// Called from Root ()'s loop, 20 times per entry -- well past the 5x bar
	// tier2_site_heat () uses to call a site hot.
	public static int HotCall (int n, bool yes)
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
		a = a - b - c - d;
		b = b * 3 + c * 5 + d * 7;
		c = c * 11 + d * 13 + a * 17;
		d = d * 19 + a * 23 + b * 29;
		a = a + b * 2 + c * 4 + d * 8;
		b = b - c * 3 - d * 5 - a * 7;
		c = c ^ (a + b) ^ (d - a);
		d = d | (a & b) | (c & a);

		if (yes)
			throw new InvalidOperationException ("hot");

		return a + b + c + d;
	}

	// The same body under a name of its own. Called from a branch Root ()'s
	// warm-up never takes, so its site's count reads near zero against Root
	// ()'s entry count once it is promoted.
	public static int ColdCall (int n, bool yes)
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
		a = a - b - c - d;
		b = b * 3 + c * 5 + d * 7;
		c = c * 11 + d * 13 + a * 17;
		d = d * 19 + a * 23 + b * 29;
		a = a + b * 2 + c * 4 + d * 8;
		b = b - c * 3 - d * 5 - a * 7;
		c = c ^ (a + b) ^ (d - a);
		d = d | (a & b) | (c & a);

		if (yes)
			throw new InvalidOperationException ("cold");

		return a + b + c + d;
	}
}

static class Program {
	static bool sawHot, sawCold, foldedHot, foldedCold;

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

	// takeRare throws on the loop's last turn (the hot site) and once more
	// outside it (the cold site), so both fire exactly once per call and each
	// catch stays inside Root ()'s own frame, the way tier2-inline-cost.cs's
	// Record () does.
	static int Root (int n, bool takeRare)
	{
		int total = 0;

		for (int i = 0; i < 20; ++i) {
			try {
				total += Costed.HotCall (n + i, takeRare && i == 19);
			} catch (InvalidOperationException e) {
				sawHot = true;
				foldedHot = RunsInsideRoot (e, "HotCall");
			}
		}

		if (takeRare) {
			try {
				Costed.ColdCall (n, true);
			} catch (InvalidOperationException e) {
				sawCold = true;
				foldedCold = RunsInsideRoot (e, "ColdCall");
			}
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

		// 20000 entries with the loop's own 20 calls each puts HotCall ()'s
		// site at 20x Root ()'s entry count, and ColdCall ()'s site at 0 --
		// taken on no call here.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		sawHot = sawCold = foldedHot = foldedCold = false;

		Root (4, true);

		Check (sawHot && sawCold, "both call sites threw at tier 2");
		Check (foldedHot,
			"the hot site folds a body past the flat default, under -hot's limit");
		Check (!foldedCold, "the cold site keeps its call, over -cold's limit");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
