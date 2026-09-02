using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * A candidate the cost model materializes gets its own tier-1 profile now,
 * not just the root's - see backend.cpp's build_profile () and
 * ProfileInliner::profile_for () (profile-inlines.cpp).
 *
 * HotWithColdThrow ()'s branch to ThrowHelper () is real: the warm-up below
 * never takes it. PromoteNow (tier 1) locks that count in before
 * ThrowHelperLoop () is promoted at all, so HotWithColdThrow () carries its
 * own record by the time it is materialized as ThrowHelperLoop ()'s
 * candidate. ThrowHelperLoop () then folds HotWithColdThrow () in - it is
 * cheap and every arm agrees on that - and the fold exposes ThrowHelper ()'s
 * call as a site inside ThrowHelperLoop () for the same pass to weigh next.
 * Answered off HotWithColdThrow ()'s own record, that site reads as never
 * taken and ThrowHelper () is declined. Answered off ThrowHelperLoop ()'s
 * record instead, the site has no entry for HotWithColdThrow ()'s own
 * branch and reads off LLVM's static estimate, which folds ThrowHelper ()
 * into the hot loop.
 *
 * The probe call after promotion passes a poison index so exactly one
 * iteration takes the throw, on the same tier-2 code the warm-up promoted -
 * the warm-up itself never passes one, so the record that promotion reads
 * still shows the branch as never taken.
 *
 * -mono-tier2-threshold=0 keeps both methods from promoting on their own, so
 * PromoteNow () alone decides when each does. -mono-inline-il-limit=0 keeps
 * the trivial pre-pass from folding ThrowHelper () on its shape before the
 * cost model ever sees it - straight-line-then-throw is exactly what that
 * pre-pass takes.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class ProfileContext {
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ThrowHelper (int code)
	{
		throw new InvalidOperationException ("code:" + code);
	}

	static int HotWithColdThrow (int n)
	{
		if (n < 0)
			ThrowHelper (n);
		return n * 2 + 1;
	}

	static long ThrowHelperLoop (int n, int poison)
	{
		long acc = 0;
		for (int i = 0; i < n; i++) {
			int v = i == poison ? -1 : i;
			acc += HotWithColdThrow (v);
		}
		return acc;
	}

	static bool RunsInsideRoot (Exception e, string helper)
	{
		StackTrace st = new StackTrace (e, false);
		int in_helper = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "ProfileContext" && m.Name == helper)
				in_helper = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "ProfileContext" && m.Name == "ThrowHelperLoop")
				in_root = f.GetNativeOffset ();
		}

		return in_helper >= 0 && in_helper == in_root;
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
		MethodInfo hot = typeof (ProfileContext).GetMethod ("HotWithColdThrow",
			BindingFlags.Static | BindingFlags.NonPublic);
		MethodInfo loop = typeof (ProfileContext).GetMethod ("ThrowHelperLoop",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!Mono.Tiering.MonoTier.PromoteNow (hot.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: HotWithColdThrow () would not compile at tier 1");
			return 1;
		}

		// Real calls, all non-negative: the branch to ThrowHelper () gathers
		// a real count of zero.
		long sum = 0;
		for (int i = 0; i < 200000; ++i)
			sum += HotWithColdThrow (i & 0x7fffffff);

		if (!Mono.Tiering.MonoTier.PromoteNow (loop.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: ThrowHelperLoop () would not compile at tier 1");
			return 1;
		}

		// A real run with no poison, so the call site's own count and every
		// count HotWithColdThrow () carries in are both real.
		sum += ThrowHelperLoop (30000, -1);

		if (!Mono.Tiering.MonoTier.PromoteNow (loop.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: ThrowHelperLoop () would not compile at tier 2");
			return 1;
		}

		bool folded_hot = false, folded_throw = false;
		bool saw_hot = false, saw_throw = false;

		try {
			ThrowHelperLoop (1, 0);
			Check (false, "the poison iteration did not throw");
		} catch (InvalidOperationException e) {
			string trace = e.StackTrace ?? "";

			saw_hot = trace.Contains ("HotWithColdThrow");
			saw_throw = trace.Contains ("ThrowHelper");
			folded_hot = RunsInsideRoot (e, "HotWithColdThrow");
			folded_throw = RunsInsideRoot (e, "ThrowHelper");
		}

		Check (saw_hot && saw_throw, "both helpers still have a frame at tier 2");
		Check (folded_hot, "HotWithColdThrow () folds into the loop - it is cheap either way");
		Check (!folded_throw,
			"ThrowHelper () stays a call - its own record reads the branch as cold");

		if (fails > 0) {
			Console.WriteLine ("{0} failure(s)", fails);
			return 1;
		}

		Console.WriteLine ("sum={0}", sum);
		return 0;
	}
}
