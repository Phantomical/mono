using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * `mono-inline-alloc-elision-bonus` and `mono-inline-alloc-elision-pending-bonus`,
 * the two threshold bonuses for a callee that does not capture a parameter the site
 * fills with a fresh allocation. The fold uncovers the accesses a call was hiding,
 * so SROA can scalarize the allocation away -- but only where the site being
 * weighed is the pointer's last reader. `alloc_elision_fate ()`
 * (passes/inline-policy.cpp) picks the bonus with one scan of the allocation's own
 * uses, skipping the site itself. It awards the full bonus where the scan finds no
 * other use, and the pending one everywhere it cannot rule the pointer out.
 *
 * Sum () and SumCold () run almost the same body. SumCold () has one more
 * Slow () call, so its own compiled body does not alias Root ()'s call-site
 * offset into it -- the stack-trace check below depends on that. What tells
 * the two sites apart is what Root () does with the argument before the call.
 * Sum ()'s allocation reaches nothing else, so its site earns the full bonus.
 * SumCold ()'s reaches Program.last first, a store the scan will not chase,
 * so its site earns only the pending one. The suite runs twice, once
 * on the defaults and once with both bonuses zeroed, and reads
 * MONO_INLINE_POLICY to know which arm it is in. The trivial pre-pass is off
 * in both (--llvm-opt=-mono-inline-il-limit=0), so a fold this reads is the
 * cost model's.
 *
 * What says a fold happened is the stack trace, the way tier2-inline-cost.cs reads
 * it: a folded body owns no code, so its frame reports the offset into Root () that
 * it was folded at, and a body that was really called reports an offset into itself.
 *
 * Sum () costs 295 and SumCold () costs 350 on -mono-inline-cost-full, on either
 * arm -- the bonus changes the budget rather than the cost. The suite names a
 * cold-callsite threshold of 100. The full bonus of 1000 clears it by a wide
 * margin (100 + 1000 = 1100 > 295), and the pending bonus of 150 does not
 * (100 + 150 = 250 < 350). So the on arm folds Sum () and still declines
 * SumCold (), which keeps the staging visible rather than merely bigger.
 * Re-measure both bodies when this starts failing on one arm:
 *
 *   MONO_LLVM_JIT_TRACE=1 mono-sgen \
 *     --llvm-opt=-mono-tier2-threshold=0 \
 *     --llvm-opt=-mono-inline-il-limit=0 \
 *     --llvm-opt=-mono-inline-cost-full \
 *     --llvm-opt=-mono-inline-cold-callsite-threshold=1 \
 *     tier2-inline-alloc-elision.exe
 *
 * -mono-inline-cost-full is what makes the printed cost the whole cost -- the model
 * stops adding once it is past the budget, so the cost a plain run prints is cut off
 * there.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Point {
	public int x, y;
}

static class Ops {
	/*
	 * Stays a call, so the arm below is work the model has to count until the
	 * fold uncovers p's fields.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Slow (int v) { return v * 3 + 1; }

	/*
	 * Never dispatches on p and never lets it escape, only reads its fields --
	 * the shape the elision bonus alone is what flips.
	 */
	public static int Sum (Point p, bool yes)
	{
		int total = p.x + p.y
		          + Slow (p.x) + Slow (p.y)
		          + Slow (p.x + 1) + Slow (p.y + 1);

		if (yes)
			throw new InvalidOperationException ("sum");

		return total;
	}

	/// The same shape as Sum (), plus one more Slow () so its compiled body
	/// does not alias Root ()'s call-site offset into it. A body of its own
	/// rather than a shared helper, so each keeps a frame the stack trace names
	/// apart.
	public static int SumCold (Point p, bool yes)
	{
		int total = p.x + p.y
		          + Slow (p.x) + Slow (p.y)
		          + Slow (p.x + 1) + Slow (p.y + 1)
		          + Slow (p.x + p.y);

		if (yes)
			throw new InvalidOperationException ("sumcold");

		return total;
	}
}

static class Program {
	/// Where SumCold () parks its allocation before the call that reads it, so
	/// the scan finds a second use and never reads the site as the pointer's
	/// last one.
	static Point last;

	static bool saw_sum, folded_sum;
	static bool saw_escape, folded_escape;

	/// Whether \p helper's frame covers the same code as Root ()'s.
	static bool RunsInsideRoot (Exception e, string helper)
	{
		StackTrace st = new StackTrace (e, false);
		int in_helper = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Ops" && m.Name == helper)
				in_helper = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_helper >= 0 && in_helper == in_root;
	}

	/*
	 * The warm-up calls all take the early return, so the profile reads the
	 * block below as cold and the model weighs its sites against
	 * ColdCallSiteThreshold. That is the range this test calibrates in.
	 */
	static int Root (int n, bool throwing)
	{
		int total = n * 2;

		if (n >= 0)
			return total;

		try {
			total += Ops.Sum (new Point { x = n & 3, y = 2 }, throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_sum |= (e.StackTrace ?? "").Contains ("Ops.Sum");
			folded_sum |= RunsInsideRoot (e, "Sum");
		}

		try {
			Point p = new Point { x = n & 3, y = 2 };

			// The extra reader alloc_elision_fate () has to see: this allocation
			// reaches Program.last as well as the call below.
			last = p;
			total += Ops.SumCold (p, throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_escape |= (e.StackTrace ?? "").Contains ("Ops.SumCold");
			folded_escape |= RunsInsideRoot (e, "SumCold");
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
		bool bonuses = Environment.GetEnvironmentVariable ("MONO_INLINE_POLICY") != "off";
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		// Tier 1 first and asked for, for the reason tier2-inline-cost.cs gives.
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (-2, true);

		Check (saw_sum, "Sum () has a frame before tier 2");
		Check (!folded_sum, "and it runs in a body of its own before tier 2");
		Check (saw_escape, "SumCold () has a frame before tier 2");
		Check (!folded_escape, "and it runs in a body of its own before tier 2");

		// Enough calls to leave counts on the tier-1 body.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_sum = folded_sum = saw_escape = folded_escape = false;

		Check (want == Root (-2, true), "the answer at tier 2 is the answer before it");
		Check (saw_sum, "Sum () still has a frame at tier 2");
		Check (saw_escape, "SumCold () still has a frame at tier 2");

		if (bonuses) {
			Check (folded_sum,
				"the full elision bonus folds a body whose argument reaches nothing else");
			Check (!folded_escape,
				"the pending bonus alone does not clear a threshold the full one is sized for");
		} else {
			Check (!folded_sum,
				"the elision bonus is what folds the body whose argument reaches nothing else");
			Check (!folded_escape,
				"neither elision bonus is standing to fold the body with the extra reader");
		}

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
