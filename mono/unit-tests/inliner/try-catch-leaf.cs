using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A clause-bearing callee at a plain call site.  No IL padding needed:
// mono_method_check_inlining () in classic mini declines any method with clauses
// outright regardless of size, so the call always survives to the top-down pass.

public class TryCatchLeaf {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (TryCatchLeaf), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int LeafTryCatch (int x) {
		try {
			if (x < 0)
				throw new ArgumentOutOfRangeException ();
			return x + 100;
		} catch (ArgumentOutOfRangeException) {
			return -x;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int TryCatchHotCaller (int x) {
		return LeafTryCatch (x);
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_try_catch_leaf_refused () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++) {
			int x = (i % 3 == 0) ? -(i % 50) : (i % 50);
			sum += TryCatchHotCaller (x);
		}
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = (i % 3 == 0) ? -(i % 50) : (i % 50);
			expected += x < 0 ? -x : x + 100;
		}
		return sum == expected ? 0 : 1;
	}
}
