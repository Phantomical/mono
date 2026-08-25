using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * A handler entered in a body that passes arguments on the stack.
 *
 * Nine integer arguments fill the six argument registers and put three on the
 * stack. Where a call site pushes those three instead, its rsp sits below the
 * rsp the body's handlers and its epilogue are written against. LLVM records
 * the difference as DW_CFA_GNU_args_size. `.mono_unwind` carries no such rule,
 * so a resume that keeps the call site's rsp enters the handler low by it. The
 * epilogue then returns through the wrong slot.
 *
 * Each of the two methods reads a stack argument inside its handler. The
 * handler then reads the wrong slot, and the answer is wrong even where the
 * return survives.
 *
 * Both methods are compiled at tier 2 through Mono.Tiering.MonoTier::PromoteNow.
 * X86CallFrameOptimization is what emits the pushes, and it does not run at
 * tier 1's CodeGenOptLevel::None. So tier 2 is the only tier that reaches this.
 * jit.cpp turns the pass off through `no-x86-call-frame-opt`.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Program {
	// MonoTier::tier2, as PromoteNow takes it.
	const int tier2 = 3;

	static int fails;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Throws (int a, int b, int c, int d, int e, int f, int g, int h, int i)
	{
		throw new InvalidOperationException ("thrown");
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Caught (int a, int b, int c, int d, int e, int f, int g, int h, int i)
	{
		try {
			return Throws (a, b, c, d, e, f, g, h, i);
		} catch (InvalidOperationException) {
			return a + b + c + d + e + f + g + h + i;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Finally (int a, int b, int c, int d, int e, int f, int g, int h, int i)
	{
		int sum = 0;

		try {
			return Throws (a, b, c, d, e, f, g, h, i);
		} finally {
			sum = a + b + c + d + e + f + g + h + i;
			Sink = sum;
		}
	}

	static int Sink;

	static MethodInfo MethodOf (string name)
	{
		return typeof (Program).GetMethod (name,
			BindingFlags.Static | BindingFlags.NonPublic);
	}

	static void Promote (string name)
	{
		if (Mono.Tiering.MonoTier.PromoteNow (MethodOf (name).MethodHandle.Value, tier2))
			return;

		Console.WriteLine ("FAIL: {0} would not compile at tier 2", name);
		++fails;
	}

	static void Check (string what, int got, int want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL: {0} answered {1}, wanted {2}", what, got, want);
		++fails;
	}

	public static int Main ()
	{
		// Warm both bodies so tier 1 has counts for the tier-2 compile.
		for (int n = 0; n < 200; ++n) {
			Caught (1, 2, 3, 4, 5, 6, 7, 8, 9);

			try {
				Finally (1, 2, 3, 4, 5, 6, 7, 8, 9);
			} catch (InvalidOperationException) {
			}
		}

		Promote ("Caught");
		Promote ("Finally");

		for (int n = 0; n < 200; ++n) {
			Check ("Caught", Caught (1, 2, 3, 4, 5, 6, 7, 8, 9), 45);

			Sink = 0;

			try {
				Finally (1, 2, 3, 4, 5, 6, 7, 8, 9);
			} catch (InvalidOperationException) {
			}

			Check ("Finally", Sink, 45);
		}

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
