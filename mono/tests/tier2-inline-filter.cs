using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Whether the tier-2 cost model can weigh a filter-bearing callee without
 * aborting the compile.
 *
 * WithFilter ()'s catch carries a filter, which the front end lowers to a
 * function of its own tied to WithFilter ()'s frame through llvm.localescape
 * and llvm.localrecover. getInlineCost refuses to fold a body carrying
 * llvm.localescape, so ProfileInliner::materialize () has to decline this
 * candidate itself. Translating it anyway would leave the filter function
 * standing once StripInlineCopiesPass takes the untaken copy back off, its
 * llvm.localrecover still naming the llvm.localescape the strip erased.
 *
 * Root () calls WithFilter () directly, so the tier-2 top-down inliner
 * considers it as a depth-1 candidate on every compile this test drives.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Foo { }

static class Clauses {
	public static int caught_locally;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool Filter (bool takeClause) { return takeClause; }

	public static int WithFilter (Foo f, bool takeClause, bool throwing)
	{
		try {
			if (throwing)
				throw new InvalidOperationException ("with-filter");

			return f == null ? 0 : 1;
		} catch (InvalidOperationException) when (Filter (takeClause)) {
			caught_locally++;
			return -1;
		}
	}
}

static class Program {
	static bool saw_propagated;

	static bool RunsInsideRoot (Exception e)
	{
		StackTrace st = new StackTrace (e, false);
		int in_helper = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Clauses" && m.Name == "WithFilter")
				in_helper = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_helper >= 0 && in_helper == in_root;
	}

	static int Root (bool takeClause, bool throwing)
	{
		Foo f = new Foo ();

		try {
			return Clauses.WithFilter (f, takeClause, throwing);
		} catch (InvalidOperationException e) {
			saw_propagated = true;

			if (RunsInsideRoot (e))
				throw new Exception ("WithFilter () folded into Root ()");
			if (!e.StackTrace.Contains ("Program.Root"))
				throw new Exception ("the frame that caught it is missing: " + e.StackTrace);

			return -2;
		}
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

		Check (Root (false, false) == 1, "the try body runs when nothing throws");
		Check (Root (true, true) == -1, "a matching filter catches locally");
		Check (Clauses.caught_locally == 1, "the local catch ran once");

		saw_propagated = false;
		Check (Root (false, true) == -2, "a filter that declines lets the throw reach Root ()");
		Check (saw_propagated, "Root () saw the exception the filter declined");

		for (int i = 0; i < 20000; ++i)
			Root (false, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		Check (Root (false, false) == 1, "the try body still runs at tier 2");
		Check (Root (true, true) == -1, "a matching filter still catches locally at tier 2");

		saw_propagated = false;
		Check (Root (false, true) == -2, "a declining filter still lets the throw reach Root () at tier 2");
		Check (saw_propagated, "Root () still sees the exception at tier 2");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
