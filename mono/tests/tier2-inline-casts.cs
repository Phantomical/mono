using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * What the tier-2 cost model answers about a type test over a parameter.
 *
 * A cascade of tests picking one implementation out of several is the shape a
 * LINQ builder has, and the tests themselves are cheap: what costs is the arms
 * behind them. The class the caller settled the operand to decides each test,
 * and the arms it rules out stop being counted.
 *
 * The suite runs twice, once on the defaults and once with the answer off, and
 * reads MONO_INLINE_POLICY to know which arm it is in. The trivial pre-pass is
 * off in both (MONO_LLVM_JIT_INLINE_IL_LIMIT=0), so a fold this reads is the
 * cost model's.
 *
 * What says a fold happened is the stack trace, the way tier2-inline-cost.cs
 * reads it: a folded body owns no code, so its frame reports the offset into
 * Root () that it was folded at, and a body that was really called reports an
 * offset into itself.
 *
 * Weigh () costs 100 on mono's answers and 850 on LLVM's, so a threshold of 400
 * leaves each verdict several hundred clear. Both arms raise
 * MONO_LLVM_JIT_INLINE_COST_IL_LIMIT, because the limit counts the IL a body
 * arrives with and this one is past the default -- and a body the model never
 * weighed prints no verdict at all, which reads as one it accepted. Re-measure
 * when this starts failing on one arm:
 *
 *   MONO_LLVM_JIT_TRACE=1 MONO_LLVM_JIT_TIER2_THRESHOLD=0 \
 *   MONO_LLVM_JIT_INLINE_IL_LIMIT=0 MONO_LLVM_JIT_INLINE_COST_IL_LIMIT=512 \
 *   mono-sgen --llvm-opt=-mono-inline-cost-full \
 *     --llvm-opt=-mono-inline-cold-callsite-threshold=1 tier2-inline-casts.exe
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

interface IMarker { }

class Holder {
	public int w;

	public Holder (int a) { w = a; }
}

sealed class Other : IMarker { }

static class Work {
	/*
	 * Stays a call, so each arm is work the model counts until the test in
	 * front of it is settled.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Slow (int x) { return x * 3 + 1; }

	/*
	 * A cascade of tests over a parameter the site fills with a Holder. Every
	 * one of them answers no once the operand's class is known, and the four
	 * arms behind them cannot run.
	 */
	public static int Weigh (object o, bool yes)
	{
		int total = 1;

		if (o is string)
			total += Slow (1) + Slow (2) + Slow (3);
		else if (o is int[])
			total += Slow (4) + Slow (5) + Slow (6);
		else if (o is IMarker)
			total += Slow (7) + Slow (8) + Slow (9);
		else if (o is Other)
			total += Slow (10) + Slow (11) + Slow (12);
		else
			total += ((Holder) o).w;

		if (yes)
			throw new InvalidOperationException ("weigh");

		return total;
	}
}

static class Program {
	/* Whether Weigh () had a frame at all, and whether it ran inside Root (). */
	static bool saw_weigh, folded_weigh;

	/// Whether Weigh ()'s frame covers the same code as Root ()'s.
	static bool RunsInsideRoot (Exception e)
	{
		StackTrace st = new StackTrace (e, false);
		int in_weigh = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Work" && m.Name == "Weigh")
				in_weigh = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_weigh >= 0 && in_weigh == in_root;
	}

	/*
	 * The warm-up calls all take the early return, so the profile reads the
	 * block below as cold and the model weighs its site against
	 * ColdCallSiteThreshold. That is the range this test calibrates in.
	 */
	static int Root (int n, bool throwing)
	{
		int total = n * 2;

		if (n >= 0)
			return total;

		try {
			total += Work.Weigh (new Holder (n & 3), throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_weigh |= (e.StackTrace ?? "").Contains ("Work.Weigh");
			folded_weigh |= RunsInsideRoot (e);
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
		bool answers = Environment.GetEnvironmentVariable ("MONO_INLINE_POLICY") != "off";
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		// Tier 1 first and asked for, for the reason tier2-inline-cost.cs gives.
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (-2, true);

		Check (saw_weigh, "Weigh () has a frame before tier 2");
		Check (!folded_weigh, "and it runs in a body of its own before tier 2");

		// Enough calls to leave counts on the tier-1 body.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_weigh = folded_weigh = false;

		Check (want == Root (-2, true), "the answer at tier 2 is the answer before it");
		Check (saw_weigh, "Weigh () still has a frame at tier 2");

		if (answers)
			Check (folded_weigh, "an answered cascade folds the body behind it");
		else
			Check (!folded_weigh,
				"an answered cascade is what folds the body behind it");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
