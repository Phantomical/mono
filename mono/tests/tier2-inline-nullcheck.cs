using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * `mono-inline-implicit-null-free` leaves the raising arm of a folded null
 * check out of a callee's cost, because ImplicitNullChecks folds the test
 * into the dereference and mono raises from the faulting instruction rather
 * than entering the handler.
 *
 * Unlike the bonuses in tier2-inline-policy.cs, this is a cost change rather
 * than a threshold change, so the two arms differ in what the model counts
 * for the same callee rather than in what it is willing to spend. The suite
 * runs twice, once on the default and once with the option off, and reads
 * MONO_INLINE_POLICY to know which arm it is in. The trivial pre-pass is off
 * in both (MONO_LLVM_JIT_INLINE_IL_LIMIT=0), so a fold this reads is the cost
 * model's.
 *
 * What says a fold happened is the stack trace, the way tier2-inline-cost.cs
 * reads it: a folded body owns no code, so its frame reports the offset into
 * Root () that it was folded at, and a body that was really called reports an
 * offset into itself.
 *
 * Walk () holds eighteen dereferences, at code size past the 128-byte default
 * MONO_LLVM_JIT_INLINE_COST_IL_LIMIT, so both arms raise it. With
 * -mono-inline-cost-full, the option costs the body 380 and leaving it off
 * costs 1010 -- the 630 gap is the eighteen raising arms, each an
 * InvalidOperationException-shaped throw once counted. Both figures are past
 * 225, the base threshold's default, and raising -mono-inline-cold-callsite-
 * threshold alone would not help: MinIfValid takes the lesser of it and the
 * base threshold. So the suite raises both -- -mono-inlinedefault-threshold
 * at 1200 together with -mono-inline-cold-callsite-threshold at 700 leave
 * 320 either side of both costs. Re-measure all of it when this starts
 * failing on one arm:
 *
 *   MONO_LLVM_JIT_TRACE=1 MONO_LLVM_JIT_TIER2_THRESHOLD=0 \
 *   MONO_LLVM_JIT_INLINE_IL_LIMIT=0 MONO_LLVM_JIT_INLINE_COST_IL_LIMIT=512 \
 *   mono-sgen --llvm-opt=-mono-inline-cost-full \
 *     --llvm-opt=-mono-inline-cold-callsite-threshold=1 \
 *     tier2-inline-nullcheck.exe
 *
 * That gives the on arm's cost. Add
 * --llvm-opt=-mono-inline-implicit-null-free=false for the off arm's.
 * -mono-inline-cost-full is what makes the printed cost the whole cost --
 * the model stops adding once it is past the budget, so the cost a plain run
 * prints is cut off there.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Node {
	public int v;
	public Node n;
}

static class Chain {
	// Six variables at depth three: eighteen implicit null checks.
	public static int Walk (Node a, Node b, Node c, Node d, Node e, Node f, bool yes)
	{
		int total = a.v + a.n.v + a.n.n.v
		          + b.v * 3 + b.n.v * 5 + b.n.n.v * 7
		          + c.v * 11 + c.n.v * 13 + c.n.n.v * 17
		          + d.v * 19 + d.n.v * 23 + d.n.n.v * 29
		          + e.v * 31 + e.n.v * 37 + e.n.n.v * 41
		          + f.v * 43 + f.n.v * 47 + f.n.n.v * 53;

		if (yes)
			throw new InvalidOperationException ("walk");

		return total;
	}
}

static class Program {
	static bool saw_walk, folded_walk;

	static Node mk (int b) { return new Node { v = b, n = new Node { v = b + 1, n = new Node { v = b + 2 } } }; }

	static Node p = mk (1), q = mk (4), r = mk (7), s = mk (10), t = mk (13), u = mk (16);

	/// Whether Walk ()'s frame covers the same code as Root ()'s.
	static bool RunsInsideRoot (Exception e)
	{
		StackTrace st = new StackTrace (e, false);
		int in_walk = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Chain" && m.Name == "Walk")
				in_walk = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_walk >= 0 && in_walk == in_root;
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
			total += Chain.Walk (p, q, r, s, t, u, throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_walk |= (e.StackTrace ?? "").Contains ("Chain.Walk");
			folded_walk |= RunsInsideRoot (e);
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
		bool nullfree = Environment.GetEnvironmentVariable ("MONO_INLINE_POLICY") != "off";
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		// Tier 1 first and asked for, for the reason tier2-inline-cost.cs gives.
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (-2, true);

		Check (saw_walk, "Walk () has a frame before tier 2");
		Check (!folded_walk, "and it runs in a body of its own before tier 2");

		// Enough calls to leave counts on the tier-1 body.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_walk = folded_walk = false;

		Check (want == Root (-2, true), "the answer at tier 2 is the answer before it");
		Check (saw_walk, "Walk () still has a frame at tier 2");

		if (nullfree)
			Check (folded_walk,
				"leaving the raising arms uncounted is what folds a body of eighteen null checks");
		else
			Check (!folded_walk,
				"counting the raising arms is what keeps the body of eighteen null checks declined");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
