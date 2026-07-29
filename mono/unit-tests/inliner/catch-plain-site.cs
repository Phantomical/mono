using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A catch-bearing callee at a plain site.  Same blockaddress limit as
// finally-plain-site.cs: it throws, so it reaches LLVM's cost model and stops there.

public class CatchPlainSite {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (CatchPlainSite), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int CatchLeaf (int x) {
		try {
			if (x == 3)
				throw new InvalidOperationException ("three");
			return x;
		} catch (InvalidOperationException) {
			return -1;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CatchHotCaller (int x) {
		return CatchLeaf (x);
	}

	// INLINER-EXPECT: exposed CatchPlainSite:CatchLeaf (int)
	public static int test_0_catch_callee_folded_at_plain_site () {
		int ITERS = Iters ();
		int sum = 0;

		for (int i = 0; i < ITERS; i++)
			sum += CatchHotCaller (i % 10);

		return sum == ExpectedCatchSum (ITERS) ? 0 : 1;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	static int ExpectedCatchSum (int iters) {
		int sum = 0;
		for (int i = 0; i < iters; i++) {
			int x = i % 10;
			sum += x == 3 ? -1 : x;
		}
		return sum;
	}
}
