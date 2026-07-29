using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// FinallyLeaf touches memory, so it carries an implicit null/bounds check, and
// emit_cond_throw () passes a blockaddress to the throw trampoline for those.  LLVM's
// isInlineViable () hard-refuses any function with a taken block address, so this
// reaches its cost model and stops there -- a general tier-1 limit that has nothing to
// do with EH (pure-finally.cs is the same shape without the checks, and folds).  It
// stays as differential coverage of the clause paths themselves.

public class FinallyPlainSite {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (FinallyPlainSite), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int FinallyLeaf (int x, int[] log) {
		try {
			if (x == 7)
				return 100;
			return x;
		} finally {
			log [0] ++;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FinallyHotCaller (int x, int[] log) {
		return FinallyLeaf (x, log);
	}

	// INLINER-EXPECT: exposed FinallyPlainSite:FinallyLeaf (int,int[])
	public static int test_0_finally_callee_folded_at_plain_site () {
		int ITERS = Iters ();
		int[] log = new int [1];
		int sum = 0;

		for (int i = 0; i < ITERS; i++)
			sum += FinallyHotCaller (i % 10, log);

		// The finally has to have run once per call, and the x==7 early return still
		// has to leave through it with the value it returned.
		if (log [0] != ITERS)
			return 1;
		return sum == ExpectedFinallySum (ITERS) ? 0 : 2;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	static int ExpectedFinallySum (int iters) {
		int sum = 0;
		for (int i = 0; i < iters; i++) {
			int x = i % 10;
			sum += x == 7 ? 100 : x;
		}
		return sum;
	}
}
