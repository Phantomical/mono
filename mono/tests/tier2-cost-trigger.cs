using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

/*
 * The counter that takes a body to tier 2 on the work it does rather than on
 * the number of times it is entered.
 *
 * Kernel () is called far fewer times than MONO_LLVM_JIT_TIER2_THRESHOLD, and
 * each call runs a loop long enough to spend the work threshold. A promotion
 * here is therefore the work counter's and no other counter's.
 *
 * What says the tier-2 body is the one running is the stack trace. Probe () has
 * a branch, so the shape-test pre-pass declines it and only the tier-2 cost
 * model folds it in. A folded body owns no code: its frame reports the native
 * offset of the call site in Kernel () that it was folded at, and the same
 * helper called for real reports an offset into its own body.
 *
 * The suite registers this source twice. MONO_LLVM_JIT_TIER2_COST_THRESHOLD
 * names the work threshold in one arm and is zero in the other, which leaves
 * the body counting entries alone. The test reads the variable and asserts the
 * arm it is in, so the second arm is this one's negative control.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Program {
	/* MonoTier::tier1, as PromoteNow takes it. */
	const int tier1 = 2;

	/* Turns of the loop in one call of Kernel (). */
	const int work = 100000;

	/*
	 * Calls of Kernel () before the test looks at the tier. Three orders of
	 * magnitude below the default entry threshold of twenty thousand, so the
	 * entry counter cannot be what promotes this body.
	 */
	const int calls = 64;

	/* Whether Probe () had a frame in the last trace, and where its code was. */
	static bool saw_probe, probe_runs_inside_kernel;

	// A branch, so the shape-test pre-pass declines this body and the tier-2
	// cost model is the only thing that folds it in.
	static void Probe (string what, bool yes)
	{
		if (yes)
			throw new InvalidOperationException (what);
	}

	/*
	 * Whether the frame reported for Probe () covers the same code as
	 * Kernel ()'s.
	 *
	 * A folded body has no code of its own, so the frame built for it reports
	 * the call site in Kernel () that it was folded at. That is the same native
	 * offset Kernel ()'s own frame reports. A helper that was really called runs
	 * in its own body and answers with an offset into that.
	 */
	static bool RunsInsideKernel (Exception e)
	{
		StackTrace st = new StackTrace (e, false);
		int in_probe = -1, in_kernel = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null || m.DeclaringType.Name != "Program")
				continue;
			if (m.Name == "Probe")
				in_probe = f.GetNativeOffset ();
			if (m.Name == "Kernel")
				in_kernel = f.GetNativeOffset ();
		}

		return in_probe >= 0 && in_probe == in_kernel;
	}

	static int Kernel (int n, bool throwing)
	{
		int total = 0;

		/*
		 * The work counter counts the turns of this loop. An entry counter sees
		 * one call whatever the loop runs, which is the split this test is
		 * about.
		 *
		 * Probe () is called from inside the loop rather than after it. The
		 * cost model ranks a call site by the block count the profile gave it,
		 * and a site beside a loop this long reads cold whatever the loop
		 * costs. Inside it the site is as hot as the loop, which is what leaves
		 * the fold to the gates and to the size of the body.
		 */
		try {
			for (int i = 0; i < n; ++i) {
				total += i * 3 + (i & 7);
				Probe ("probe", throwing);
			}
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_probe = true;
			probe_runs_inside_kernel = RunsInsideKernel (e);
		}

		return total;
	}

	/* One call that throws, and what its trace said about the tier. */
	static bool Observe ()
	{
		saw_probe = probe_runs_inside_kernel = false;
		Kernel (work, true);

		if (!saw_probe)
			throw new Exception ("Probe () has no frame in the trace");

		return probe_runs_inside_kernel;
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
		string threshold = Environment.GetEnvironmentVariable (
			"MONO_LLVM_JIT_TIER2_COST_THRESHOLD");
		bool want_tier2 = threshold != "0";

		MethodInfo kernel = typeof (Program).GetMethod ("Kernel",
			BindingFlags.Static | BindingFlags.NonPublic);

		/*
		 * Tier 1 first, and asked for rather than waited for: an interpreted
		 * caller reaches an interpreted callee without the runtime being asked
		 * for it, so the calls below leave Kernel () where it started. The work
		 * counter is in the tier-1 body and counts nothing until there is one.
		 */
		if (!Mono.Tiering.MonoTier.PromoteNow (kernel.MethodHandle.Value, tier1)) {
			Console.WriteLine ("FAIL: Kernel () would not compile at tier 1");
			return 1;
		}

		Check (!Observe (), "the helper has a body of its own before tier 2");

		int want = Kernel (work, false);

		for (int i = 0; i < calls; ++i)
			Check (Kernel (work, false) == want, "the answer stays the same");

		/*
		 * The promotion is queued, so the tier-2 body lands on a compile worker
		 * rather than on this thread. Give it a bounded wait. Each try is
		 * another call of Kernel (), which keeps the entry count far below the
		 * entry threshold in both arms.
		 */
		bool folded = false;

		for (int i = 0; i < 100 && !folded; ++i) {
			folded = Observe ();
			if (!folded)
				Thread.Sleep (10);
		}

		if (want_tier2)
			Check (folded, "the work a body does takes it to tier 2");
		else
			Check (!folded, "and a body counting entries alone stays at tier 1");

		Check (Kernel (work, false) == want, "the answer at the end is the answer at the start");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
