using System;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	//
	// Promotion-policy probe, backed by JIT-only internal calls registered in
	// mini-runtime.c. Lets the test assert the POLICY (a cold method stays tier
	// 0; a hot one is promoted), which semantic-only checks cannot catch.
	//
	static class Probe {
		// MONO_TIERED_CALL_THRESHOLD, or 0 when deferred promotion is off
		// (MONO_TIERED unset, or threshold 0 = eager). Policy assertions are only
		// meaningful when this is > 0.
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();

		// Recorded tier state of the method whose MonoMethod* is METHOD (obtained
		// from RuntimeMethodHandle.Value): 1 = promoted to tier 1, 0 = queued at
		// tier 0, 2 = tier-0 terminal (declined), -1 = no record, i.e. never
		// enqueued - the method never crossed the threshold and stayed cold.
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern int MethodState (IntPtr method);
	}
}

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

	// -- Promotion-policy assertion (review NIT-3) --------------------------
	//
	// The tests above only prove semantics are preserved across promotion; they
	// pass even if the threshold were silently ignored. This one asserts the
	// POLICY itself: a hot method IS promoted and a below-threshold method is
	// NOT. It reads tier state through the MonoTests.Tiering.Probe internal
	// calls.
	//
	// Only meaningful when deferred promotion is active (threshold > 0). Under
	// the classic JIT (tiering off) or eager mode (threshold 0) the probe
	// reports "off" and there is no deferred policy to check, so the test
	// degrades to a no-op and still passes on every path and threshold.

	const int STATE_PROMOTED = 1;
	const int POLICY_COLD_CALLS = 3;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PolicyHot (int x) { return x + 1; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PolicyCold (int x) { return x - 1; }

	static IntPtr HandleOf (string name) {
		MethodInfo mi = typeof (TieredPromotion).GetMethod (
			name, BindingFlags.NonPublic | BindingFlags.Static);
		return mi.MethodHandle.Value;
	}

	public static int test_0_promotion_policy () {
		uint threshold = MonoTests.Tiering.Probe.Threshold ();
		if (threshold == 0)
			return 0;			// off / eager: no deferred policy to assert.

		// Drive PolicyHot far past any tested threshold; it must promote.
		long acc = 0;
		for (int i = 0; i < 5000; i++)
			acc += PolicyHot (i & 63);
		if (MonoTests.Tiering.Probe.MethodState (HandleOf ("PolicyHot")) != STATE_PROMOTED)
			return 1;			// a hot method was NOT promoted - policy regressed.

		// Enter PolicyCold only a handful of times. When the threshold is above
		// that count it can never cross, so it must NOT be promoted.
		int r = 0;
		for (int i = 0; i < POLICY_COLD_CALLS; i++)
			r += PolicyCold (i);
		if (threshold > POLICY_COLD_CALLS &&
		    MonoTests.Tiering.Probe.MethodState (HandleOf ("PolicyCold")) == STATE_PROMOTED)
			return 2;			// a cold method was promoted - threshold ignored.

		return (acc >= 0 && r != 0x7fffffff) ? 0 : 3;
	}
}
