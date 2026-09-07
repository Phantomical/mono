using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Which method a folded finally's `.mono_guards` record belongs to.
 *
 * Guarded<T> is the shape that gets that wrong when a marker's owner is
 * inferred rather than carried on it (mono_lsda_format.hpp). Each
 * instantiation is a method of its own with a clause 0 of its own, and they
 * emit one finally body between them over one frame slot. Their markers are
 * then identical machine instructions, and BranchFolding merges them where
 * they sit together in the function's tail.
 *
 * Root () declares no clause of its own, which is what turns that into a
 * failure rather than a silently wrong guard. Every clause in the compiled
 * body arrives through a fold, so a marker read back as the root's own names a
 * clause index the root's header does not have.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Cleanups {
	public static int ran;

	// Not inlinable, so the try region keeps a call that can unwind and the
	// clause stays live through the fold.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Work (int n)
	{
		return n + 1;
	}

	public static int Guarded<T> (T witness, int n)
	{
		try {
			return Work (n);
		} finally {
			ran++;
		}
	}
}

static class Program {
	static int Root (int n)
	{
		// Value-type instantiations, so a method with a clause each and no
		// shared body between them.
		return Cleanups.Guarded<int> (0, n)
		     + Cleanups.Guarded<long> (0L, n)
		     + Cleanups.Guarded<double> (0.0, n)
		     + Cleanups.Guarded<short> (0, n);
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

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (7);

		Check (want == 4 * 8, "the four guarded calls answer before tier 2");
		Check (Cleanups.ran == 4, "and each ran its finally once");

		for (int i = 0; i < 20000; ++i)
			Root (i);

		// The tier-2 compile is what reads the guards back, so this is the
		// call that aborts in register_jit_info () when a guard names a
		// clause Root () does not have.
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 4)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		int before = Cleanups.ran;

		Check (want == Root (7), "the answer at tier 2 is the answer before it");
		Check (Cleanups.ran == before + 4, "and each finally still runs once");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
