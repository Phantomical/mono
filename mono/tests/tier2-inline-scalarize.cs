using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * `mono-inline-scalarize-arg-bonus`, the threshold bonus for a callee that
 * does not capture a parameter the site fills with a fresh allocation. The
 * fold uncovers the accesses a call was hiding, which is what lets SROA
 * scalarize the allocation away.
 *
 * Sum () only reads p's fields and never dispatches on it, so it takes no
 * bonus but this one -- tier2-inline-policy.cs's Measure () takes the
 * argument bonus instead, because it dispatches on the parameter, and the
 * two shapes tell the bonuses apart. The suite runs twice, once on the
 * default and once with the bonus zeroed, and reads MONO_INLINE_POLICY to
 * know which arm it is in. The trivial pre-pass is off in both
 * (--llvm-opt=-mono-inline-il-limit=0), so a fold this reads is the cost
 * model's.
 *
 * What says a fold happened is the stack trace, the way tier2-inline-cost.cs
 * reads it: a folded body owns no code, so its frame reports the offset into
 * Root () that it was folded at, and a body that was really called reports an
 * offset into itself.
 *
 * Sum () costs 295 on -mono-inline-cost-full, on either arm -- the bonus
 * changes the budget rather than the cost. The suite names a cold-callsite
 * threshold of 200, which the bonus of 150 takes to 350 in the on arm and
 * leaves at 200 in the off arm, so both verdicts have around a hundred either
 * way. Re-measure when this starts failing on one arm:
 *
 *   MONO_LLVM_JIT_TRACE=1 mono-sgen \
 *     --llvm-opt=-mono-tier2-threshold=0 \
 *     --llvm-opt=-mono-inline-il-limit=0 \
 *     --llvm-opt=-mono-inline-cost-full \
 *     --llvm-opt=-mono-inline-cold-callsite-threshold=1 \
 *     tier2-inline-scalarize.exe
 *
 * -mono-inline-cost-full is what makes the printed cost the whole cost -- the
 * model stops adding once it is past the budget, so the cost a plain run
 * prints is cut off there.
 *
 * That the allocation is what SROA actually takes the fold's uncovered
 * accesses away on, rather than the bonus paying for a scalarization that
 * never happens, is checked separately with MONO_JIT_DUMP=tier2-ir: the on
 * arm's Root () holds no allocator call for Point at all, where the
 * exception's own allocation still has one, and Point's fields have become
 * SSA values the arithmetic reads directly.
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
	 * the shape the scalarize bonus alone is what flips.
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
}

static class Program {
	static bool saw_sum, folded_sum;

	/// Whether Sum ()'s frame covers the same code as Root ()'s.
	static bool RunsInsideRoot (Exception e)
	{
		StackTrace st = new StackTrace (e, false);
		int in_sum = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Ops" && m.Name == "Sum")
				in_sum = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_sum >= 0 && in_sum == in_root;
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
			total += Ops.Sum (new Point { x = n & 3, y = 2 }, throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_sum |= (e.StackTrace ?? "").Contains ("Ops.Sum");
			folded_sum |= RunsInsideRoot (e);
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
		bool scalarize = Environment.GetEnvironmentVariable ("MONO_INLINE_POLICY") != "off";
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

		// Enough calls to leave counts on the tier-1 body.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_sum = folded_sum = false;

		Check (want == Root (-2, true), "the answer at tier 2 is the answer before it");
		Check (saw_sum, "Sum () still has a frame at tier 2");

		if (scalarize)
			Check (folded_sum,
				"the scalarize bonus folds a body that only reads an uncaptured argument's fields");
		else
			Check (!folded_sum,
				"the scalarize bonus is what folds the body that reads the argument's fields");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
