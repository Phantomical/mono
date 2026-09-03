using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * The callees tier 2 weighs before folding them in, which is everything the
 * shape test in front of the pre-pass refuses: a branch, a loop, several calls.
 *
 * Root () is called until it has run at tier 1 long enough to have counts, and
 * Mono.Tiering.MonoTier::PromoteNow then compiles it at tier 2 on this thread
 * against them. The suite runs with --llvm-opt=-mono-tier2-threshold=0, so
 * the body stays instrumented and counting and never promotes on its own -
 * which is what keeps the compile this test is about the only one there is.
 *
 * What says a fold really happened is the stack trace. Every helper that threw
 * has a frame in it either way, but a folded body owns no code: its frame
 * reports the offset into Root () that it was folded at, and a helper the gates
 * refuse reports an offset into itself. The warm-up calls ask the helpers not to
 * throw, because the trace is the expensive part and the calls are what the
 * counts are wanted for.
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
	 * A body that calls a NoInlining method. The mark keeps FailNoInline () out
	 * of every fold, so that method holds a body and a frame of its own whatever
	 * its caller does. The mark says nothing about this body, and the cost model
	 * is free to fold it. The branch keeps the shape test off this body, so a
	 * fold here is the cost model's.
	 */
	public static void FailThroughNoInline (string what, bool yes)
	{
		if (yes)
			FailNoInline (what, true);
	}

	/* What GetCurrentMethod () named inside FailThroughFrameRead (). */
	public static string frame_read_saw;

	/*
	 * A body whose own calls read the frame they were called from. The fold takes
	 * this body's frame off the stack, so both icalls answer off the frames the
	 * compile recorded instead. The name below has to stay this method's, folded
	 * or not, and the branch keeps the shape test away so a fold here is the cost
	 * model's.
	 */
	public static void FailThroughFrameRead (string what, bool yes)
	{
		if (yes && Assembly.GetCallingAssembly () != null) {
			frame_read_saw = MethodBase.GetCurrentMethod ().Name;
			throw new InvalidOperationException (what);
		}
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
	static bool saw_branch, saw_no_inline, saw_through, saw_frame_read;

	/* Which of them ran inside Root ()'s code rather than in a body of its own. */
	static bool folded_branch, folded_no_inline, folded_through, folded_frame_read;

	/*
	 * Whether the helper's frame covers the same code as Root ()'s.
	 *
	 * A folded body has no code of its own, so the frame reported for it names
	 * the call site in Root () that it was folded at - the same native offset
	 * Root ()'s own frame reports. A helper that was really called runs in its
	 * own body and reports an offset into that.
	 */
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

	static void Record (Exception e)
	{
		string trace = e.StackTrace ?? "";

		saw_branch |= trace.Contains ("Costed.FailBranch");
		saw_no_inline |= trace.Contains ("Costed.FailNoInline");
		saw_through |= trace.Contains ("Costed.FailThroughNoInline");
		saw_frame_read |= trace.Contains ("Costed.FailThroughFrameRead");

		folded_branch |= RunsInsideRoot (e, "FailBranch");
		folded_no_inline |= RunsInsideRoot (e, "FailNoInline");
		folded_through |= RunsInsideRoot (e, "FailThroughNoInline");
		folded_frame_read |= RunsInsideRoot (e, "FailThroughFrameRead");

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
			Costed.FailThroughFrameRead ("read", throwing);
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

		Check (saw_branch && saw_no_inline && saw_through && saw_frame_read,
			"every helper has a frame before tier 2");
		Check (!folded_branch && !folded_no_inline && !folded_through
			&& !folded_frame_read,
			"and every one of them runs in a body of its own before tier 2");
		Check (Costed.frame_read_saw == "FailThroughFrameRead",
			"GetCurrentMethod () names the body that asked before tier 2, not "
			+ (Costed.frame_read_saw ?? "nothing"));

		// Enough calls to leave counts on the tier-1 body, and every one of them
		// through the same branch, so the site Rare () sits behind reads cold.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_branch = saw_no_inline = saw_through = saw_frame_read = false;
		folded_branch = folded_no_inline = folded_through = folded_frame_read = false;
		Costed.frame_read_saw = null;

		Check (want == Root (4, true), "the answer at tier 2 is the answer before it");

		/*
		 * A folded body keeps a frame in the trace, built from the side table
		 * the compile wrote rather than from a frame on the stack. What says the
		 * fold happened is where that frame's code is.
		 */
		Check (folded_branch, "the cost model folds a helper with a branch");
		Check (saw_no_inline && !folded_no_inline, "NoInlining keeps the helper's body");
		Check (saw_through && folded_through,
			"and the mark on it leaves a helper that calls it foldable");
		Check (saw_frame_read && folded_frame_read,
			"the cost model folds a helper that reads the frame that called it");
		Check (Costed.frame_read_saw == "FailThroughFrameRead",
			"and GetCurrentMethod () still names that body, not "
			+ (Costed.frame_read_saw ?? "nothing"));

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
