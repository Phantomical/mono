using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * `mono-inline-noreturn-free` leaves an arm whose every exit reaches a
 * noreturn call out of a callee's cost, generalizing
 * `mono-inline-implicit-null-free` past the shape of a folded null check: a
 * guard that raises through an ordinary comparison, not a dereference, is
 * the same shape once ImplicitNullChecks folds a dereference's own check
 * into the load. tier2-inline-nullcheck.cs is that first shape. This file
 * is the general one, and its guards hold no dereference at all.
 *
 * The suite runs twice, once on the default and once with the option off,
 * and reads MONO_INLINE_POLICY to know which arm it is in. The trivial
 * pre-pass is off in both (--llvm-opt=-mono-inline-il-limit=0), so a fold
 * this reads is the cost model's.
 *
 * What says a fold happened is the stack trace, the way
 * tier2-inline-nullcheck.cs reads it: a folded body owns no code, so its
 * frame reports the offset into Root () that it was folded at, and a body
 * that was really called reports an offset into itself.
 *
 * The site is cold -- the warm-up calls all take Root ()'s early return --
 * so the model weighs it against ColdCallSiteThreshold, 45 by default.
 * Validate () raises through the six range checks and one explicit throw
 * below them. With all seven arms out, it costs 0 and folds. Counting them
 * costs 70 and declines. No suite here has to raise a threshold to keep the
 * two arms apart. Re-measure both numbers when this starts failing on one
 * arm:
 *
 *   MONO_LLVM_JIT_TRACE=1 mono-sgen --llvm-opt=-mono-tier2-threshold=0 \
 *     --llvm-opt=-mono-inline-il-limit=0 \
 *     --llvm-opt=-mono-inline-cost-full \
 *     --llvm-opt=-mono-inline-cold-callsite-threshold=1 \
 *     tier2-inline-noreturn.exe
 *
 * That gives the on arm's cost. Add
 * --llvm-opt=-mono-inline-noreturn-free=false for the off arm's.
 * -mono-inline-cost-full is what makes the printed cost the whole cost --
 * the model stops adding once it is past the budget, so the cost a plain
 * run prints is cut off there.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Guard {
	// Six range checks, each an ordinary comparison rather than a
	// dereference, so mono-inline-implicit-null-free never answers for
	// any of them.
	public static int Validate (int a, int b, int c, int d, int e, int f, bool yes)
	{
		if (a < 0 || a >= 100) throw new ArgumentOutOfRangeException ("a");
		if (b < 0 || b >= 100) throw new ArgumentOutOfRangeException ("b");
		if (c < 0 || c >= 100) throw new ArgumentOutOfRangeException ("c");
		if (d < 0 || d >= 100) throw new ArgumentOutOfRangeException ("d");
		if (e < 0 || e >= 100) throw new ArgumentOutOfRangeException ("e");
		if (f < 0 || f >= 100) throw new ArgumentOutOfRangeException ("f");

		int total = a + b * 3 + c * 5 + d * 7 + e * 11 + f * 13;

		if (yes)
			throw new InvalidOperationException ("validate");

		return total;
	}
}

static class Program {
	static bool saw_validate, folded_validate;

	/// Whether Validate ()'s frame covers the same code as Root ()'s.
	static bool RunsInsideRoot (Exception e)
	{
		StackTrace st = new StackTrace (e, false);
		int in_validate = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Guard" && m.Name == "Validate")
				in_validate = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_validate >= 0 && in_validate == in_root;
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
			total += Guard.Validate (1, 2, 3, 4, 5, 6, throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_validate |= (e.StackTrace ?? "").Contains ("Guard.Validate");
			folded_validate |= RunsInsideRoot (e);
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
		bool noreturnfree = Environment.GetEnvironmentVariable ("MONO_INLINE_POLICY") != "off";
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		// Tier 1 first and asked for, for the reason tier2-inline-cost.cs gives.
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (-2, true);

		Check (saw_validate, "Validate () has a frame before tier 2");
		Check (!folded_validate, "and it runs in a body of its own before tier 2");

		// Enough calls to leave counts on the tier-1 body.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_validate = folded_validate = false;

		Check (want == Root (-2, true), "the answer at tier 2 is the answer before it");
		Check (saw_validate, "Validate () still has a frame at tier 2");

		if (noreturnfree)
			Check (folded_validate,
				"leaving the raising arms uncounted is what folds a body of six range guards");
		else
			Check (!folded_validate,
				"counting the raising arms is what keeps the body of six range guards declined");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
