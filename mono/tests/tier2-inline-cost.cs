using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * The callees tier 2 weighs before folding them in, which is everything the
 * shape test in front of the pre-pass refuses: a branch, a loop, several calls.
 *
 * Root () is called until it has run at tier 1 long enough to have counts, and
 * Mono.Tiering.MonoTier::PromoteNow then compiles it at tier 2 on this thread
 * against them. The suite runs with MONO_LLVM_JIT_TIER2_THRESHOLD=0, so the
 * body stays instrumented and counting and never promotes on its own - which is
 * what keeps the compile this test is about the only one there is.
 *
 * What says a fold really happened is the stack trace: a folded body has no
 * frame of its own, so the helper that threw is missing from the trace taken at
 * tier 2, and the helpers the gates refuse are still in it. The warm-up calls
 * ask the helpers not to throw, because the trace is the expensive part and the
 * calls are what the counts are wanted for.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Costed {
	// A branch, so the shape test declines it and the cost model is the only
	// thing that can take it.
	public static int Pick (int n, int lo, int hi)
	{
		if (n < 0)
			return lo;

		return n > 10 ? hi : lo + hi;
	}

	// A loop, which is further past the shape test again.
	public static int Sum (int n)
	{
		int total = 0;

		for (int i = 0; i < n; ++i)
			total += i * 2;

		return total;
	}

	public static void FailBranch (string what, bool yes)
	{
		if (yes)
			throw new InvalidOperationException (what);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void FailNoInline (string what, bool yes)
	{
		if (yes)
			throw new InvalidOperationException (what);
	}

	/*
	 * A body whose own call reads the frame it was called from. Folding this
	 * would hand FailNoInline () the root's frame instead of this one's, so the
	 * gate walks a candidate's calls and refuses it.
	 */
	public static void FailThroughNoInline (string what, bool yes)
	{
		FailNoInline (what, yes);
	}

	// Past MONO_LLVM_JIT_INLINE_COST_IL_LIMIT, which bounds how much IL one
	// compile will translate in order to ask what it is worth.
	public static void FailLong (string what, int n, bool yes)
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

		if (yes && a + b + c + d != int.MinValue)
			throw new InvalidOperationException (what);
	}

	// Recursion: the loop folds a few levels and the last call is left standing,
	// which is the body the sweep has to put back on the method's thunk.
	public static int Countdown (int n)
	{
		if (n <= 0)
			return 0;

		return n + Countdown (n - 1);
	}

	// Never reached while the profile is being gathered, so its site is cold
	// when the cost model reads it.
	public static int Rare (int n)
	{
		int total = 0;

		for (int i = 0; i < n + 4; ++i)
			total += i * 3 + 1;

		return total;
	}
}

static class Program {
	/* Which of the helpers the trace taken inside Root () named. */
	static bool saw_branch, saw_no_inline, saw_through, saw_long;

	static void Record (Exception e)
	{
		string trace = e.StackTrace ?? "";

		saw_branch |= trace.Contains ("Costed.FailBranch");
		saw_no_inline |= trace.Contains ("Costed.FailNoInline");
		saw_through |= trace.Contains ("Costed.FailThroughNoInline");
		saw_long |= trace.Contains ("Costed.FailLong");

		if (!trace.Contains ("Program.Root"))
			throw new Exception ("the frame that caught it is missing: " + trace);
	}

	static int Root (int n, bool throwing)
	{
		int total = Costed.Pick (n, 2, 5) + Costed.Sum (n) + Costed.Countdown (n);

		if (n < 0)
			total += Costed.Rare (n);

		try {
			Costed.FailBranch ("branch", throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			Record (e);
		}

		try {
			Costed.FailNoInline ("noinline", throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			Record (e);
		}

		try {
			Costed.FailThroughNoInline ("through", throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			Record (e);
		}

		try {
			Costed.FailLong ("long", n, throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			Record (e);
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

		/*
		 * Tier 1 first, and asked for rather than waited for: an interpreted
		 * caller reaches an interpreted callee without the runtime ever being
		 * asked for it, so a loop alone leaves Root () where it started and the
		 * tier-2 compile below would have no counts to read.
		 */
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (4, true);

		Check (saw_branch && saw_no_inline && saw_through && saw_long,
			"every helper has a frame before tier 2");

		// Enough calls to leave counts on the tier-1 body, and every one of them
		// through the same branch, so the site Rare () sits behind reads cold.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_branch = saw_no_inline = saw_through = saw_long = false;

		Check (want == Root (4, true), "the answer at tier 2 is the answer before it");

		Check (!saw_branch, "the cost model folds a helper with a branch");
		Check (saw_no_inline, "NoInlining keeps the helper's frame");
		Check (saw_through, "a helper that calls a NoInlining helper keeps its frame");
		Check (saw_long, "a helper past the IL limit keeps its frame");

		/*
		 * The path the profile never covered. Whatever the cost model decided
		 * about the call it holds, the answer has to be the one tier 1 gave -
		 * and a body it materialized and then declined is only correct because
		 * the sweep put the call back on the callee's thunk.
		 */
		int cold = Root (-2, true);

		Check (cold == 2 + 0 + 0 + Costed.Rare (-2) + 6 + 8 + 7 + 4,
			"the cold path is the answer the helpers give");
		Check (cold == Root (-2, true), "the cold path answers the same twice");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
