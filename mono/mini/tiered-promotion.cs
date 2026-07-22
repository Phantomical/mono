using System;
using System.Runtime.CompilerServices;

//
// Functional test for deferred tier-1 promotion (MONO_TIERED_CALL_THRESHOLD).
//
// Every test method is correct regardless of whether its helpers are still at
// tier 0 or have been promoted to tier 1, so the suite passes on the classic
// JIT and under `MONO_TIERED=1 --llvm` at any threshold. Its purpose under
// tiering is to exercise the promotion path and prove it preserves semantics:
//
//   MONO_PATH=<class> MONO_TIERED=1 MONO_TIERED_CALL_THRESHOLD=20 \
//     mono --llvm --regression tiered-promotion.exe
//
// The helpers are marked NoInlining so their prologues (which carry the call
// counter) actually run on every call; an inlined helper would never count.
//
public class TieredPromotion {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (TieredPromotion), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Square (int x) { return x * x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long Fib (int n) { return n < 2 ? n : Fib (n - 1) + Fib (n - 2); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Cold (int a, int b) { return a * 3 - b; }

	// Square is entered 5000 times, far past any small threshold, so under a low
	// MONO_TIERED_CALL_THRESHOLD it is promoted partway through the loop. The sum
	// must be identical whether or not that happened.
	public static int test_0_hot_method_stable_across_promotion () {
		long sum = 0;
		for (int i = 0; i < 5000; i++)
			sum += Square (i % 100);
		// 50 full sweeps of k=0..99 of k*k: 50 * (99*100*199/6).
		long expected = 50L * (99 * 100 * 199 / 6);
		return sum == expected ? 0 : 1;
	}

	// Deep self-recursion enters Fib's prologue many times from a single
	// top-level call, so promotion can fire via the entry (not top-level-call)
	// count. The result must be stable.
	public static int test_0_recursive_method_promotion () {
		return Fib (25) == 75025 ? 0 : 1;
	}

	// A method entered only a handful of times stays tier 0 under a non-trivial
	// threshold (and is promoted eagerly at threshold 0); either way it must
	// compute the same value.
	public static int test_0_cold_method_correct () {
		int r = 0;
		r += Cold (10, 1);
		r += Cold (20, 2);
		r += Cold (30, 3);
		return r == (10 * 3 - 1) + (20 * 3 - 2) + (30 * 3 - 3) ? 0 : 1;
	}
}
