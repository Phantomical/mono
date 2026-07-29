using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A clause-bearing callee whose caller also carries clauses, so both methods' tables
// are live in the same compile -- the case root-scoped clause ids exist for.  The
// invoke-site rule itself is pinned by try-finally-leaf.cs.

public class ClauseUnderClauseCaller {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (ClauseUnderClauseCaller), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int NestedFinallyLeaf (int x, int[] log) {
		try {
			return x == 5 ? 50 : x;
		} finally {
			log [0] ++;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int NestedFinallyHotCaller (int x, int[] log) {
		try {
			return NestedFinallyLeaf (x, log);
		} catch (InvalidOperationException) {
			return -1;
		}
	}

	// INLINER-EXPECT: exposed ClauseUnderClauseCaller:NestedFinallyLeaf (int,int[])
	public static int test_0_clause_callee_under_clause_bearing_caller () {
		int ITERS = Iters ();
		int[] log = new int [1];
		int sum = 0;

		for (int i = 0; i < ITERS; i++)
			sum += NestedFinallyHotCaller (i % 10, log);

		if (log [0] != ITERS)
			return 1;
		return sum == ExpectedNestedFinallySum (ITERS) ? 0 : 2;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	static int ExpectedNestedFinallySum (int iters) {
		int sum = 0;
		for (int i = 0; i < iters; i++) {
			int x = i % 10;
			sum += x == 5 ? 50 : x;
		}
		return sum;
	}
}
