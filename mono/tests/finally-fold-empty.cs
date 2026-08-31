using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Correctness for a finally that ends up with nothing between its body
 * markers: FoldEmptyFinallyPass (mono/llvm/passes/fold-empty-finally.cpp)
 * drops the markers and the thread-abort check built around them once it
 * proves that.
 *
 * PlainLeave (), ExceptionUnwind () and Nested () use a literally empty
 * `finally { }`, which the front end gives no IL of its own - the fold's
 * simplest input. DeadStore ()'s finally has IL, but writes a local nothing
 * reads, so the fold only sees an empty body once the pipeline's own
 * simplification has removed that store.
 *
 * Whether the fold actually fired is not something a method's answer can
 * show: an empty finally behaves the same either way. fold-empty-finally-
 * tests.cpp checks that removal directly, against hand-built IR. What this
 * file exercises instead is the CFG surgery around a real compiled
 * try/finally/catch, at both tiers, with whatever debug and sequence-point
 * markers a real compile adds that a hand-built module does not.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class EmptyFinally {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int PlainLeave (int x)
	{
		try {
			return x + 1;
		} finally {
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int ExceptionUnwind (int x, bool throwing)
	{
		try {
			try {
				if (throwing)
					throw new InvalidOperationException ();
				return x + 1;
			} finally {
			}
		} catch (InvalidOperationException) {
			return x + 2;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Nested (int x, bool throwing)
	{
		try {
			try {
				try {
					if (throwing)
						throw new InvalidOperationException ();
					return x + 1;
				} finally {
				}
			} finally {
			}
		} catch (InvalidOperationException) {
			return x + 3;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int DeadStore (int x)
	{
		try {
			return x + 1;
		} finally {
			int unused = x * 2;
		}
	}
}

static class Program {
	const int tier1 = 2;
	const int tier2 = 3;

	static int fails;

	static void Check (bool condition, string what)
	{
		if (!condition) {
			Console.WriteLine ("FAILED: {0}", what);
			fails++;
		}
	}

	static void Promote (string name, int tier)
	{
		MethodInfo method = typeof (EmptyFinally).GetMethod (name,
			BindingFlags.Public | BindingFlags.Static);

		if (!Mono.Tiering.MonoTier.PromoteNow (method.MethodHandle.Value, tier))
			Check (false, name + " promoted to tier " + tier);
	}

	static void RunAll (string stage)
	{
		Check (EmptyFinally.PlainLeave (1) == 2, stage + ": PlainLeave returns past its finally");
		Check (EmptyFinally.ExceptionUnwind (1, false) == 2,
			stage + ": ExceptionUnwind's try completes normally");
		Check (EmptyFinally.ExceptionUnwind (1, true) == 3,
			stage + ": ExceptionUnwind's catch runs after an unwind through the finally");
		Check (EmptyFinally.Nested (1, false) == 2, stage + ": Nested's innermost try completes normally");
		Check (EmptyFinally.Nested (1, true) == 4,
			stage + ": Nested's catch runs after an unwind through both finallys");
		Check (EmptyFinally.DeadStore (1) == 2, stage + ": DeadStore returns past its finally");
	}

	public static int Main ()
	{
		RunAll ("interpreted");

		foreach (string name in new [] { "PlainLeave", "ExceptionUnwind", "Nested", "DeadStore" })
			Promote (name, tier1);
		RunAll ("tier 1");

		foreach (string name in new [] { "PlainLeave", "ExceptionUnwind", "Nested", "DeadStore" })
			Promote (name, tier2);
		RunAll ("tier 2");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
