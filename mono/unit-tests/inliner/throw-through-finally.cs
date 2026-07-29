using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A finally a THROW passes through, so the clause is exercised on the exceptional path
// and not just the leave path.  The throw crosses the callee's finally and is caught by
// the non-inlined caller.

public class ThrowThroughFinally {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (ThrowThroughFinally), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int ThrowThroughFinallyLeaf (int x, int[] log) {
		try {
			if (x == 4)
				throw new InvalidOperationException ("four");
			return x;
		} finally {
			log [0] ++;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ThrowThroughFinallyHotCaller (int x, int[] log) {
		return ThrowThroughFinallyLeaf (x, log);
	}

	// INLINER-EXPECT: exposed ThrowThroughFinally:ThrowThroughFinallyLeaf (int,int[])
	public static int test_0_throw_through_inlined_finally () {
		int ITERS = Iters ();
		int[] log = new int [1];
		int sum = 0, caught = 0;

		for (int i = 0; i < ITERS; i++) {
			try {
				sum += ThrowThroughFinallyHotCaller (i % 10, log);
			} catch (InvalidOperationException) {
				caught ++;
			}
		}

		// The finally runs on both paths, so once per call either way.
		if (log [0] != ITERS)
			return 1;
		if (caught != ExpectedThrowCount (ITERS))
			return 2;
		return sum == ExpectedThrowSum (ITERS) ? 0 : 3;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	static int ExpectedThrowCount (int iters) {
		int n = 0;
		for (int i = 0; i < iters; i++)
			if (i % 10 == 4)
				n ++;
		return n;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	static int ExpectedThrowSum (int iters) {
		int sum = 0;
		for (int i = 0; i < iters; i++) {
			int x = i % 10;
			if (x != 4)
				sum += x;
		}
		return sum;
	}
}
