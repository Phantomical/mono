using System;
using System.Reflection;
using System.Runtime.CompilerServices;

// Checks that removing an empty finally never changes what a try/finally/catch returns
// or throws, at either compiled tier.
//
// An empty finally behaves the same whether or not the elimination ran, so no return
// value here shows whether it fired. Matching behavior across interpreted and both
// tiers is the whole test. eliminate-empty-finally-tests.cpp checks the removal itself,
// against hand-built IR. This file exercises it against a real compiled
// try/finally/catch instead.

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

	// Its finally only empties out once the pipeline's own simplification clears
	// this dead store.
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
	// MonoTier::tier1 and MonoTier::tier2, as PromoteNow takes them.
	const int tier1 = 3;
	const int tier2 = 4;

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
