using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A callee carrying its own try/finally at a PLAIN call site is inlinable: its clauses
// come along with it, the landing pads it brought name their own clauses, and
// .mono_lsda is built from those pads rather than from any one method's IL offsets.
//
// No memory access anywhere, so no implicit null/bounds check -- see the blockaddress
// note in finally-plain-site.cs for why that matters.

public class PureFinally {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (PureFinally), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int PureFinallyLeaf (int x) {
		int r;
		try {
			r = x * 2;
		} finally {
			x = 0;
		}
		return r + x;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PureFinallyHotCaller (int x) {
		return PureFinallyLeaf (x);
	}

	// INLINER-EXPECT: folded PureFinally:PureFinallyLeaf (int)
	public static int test_0_pure_finally_callee_folded () {
		int ITERS = Iters ();
		int sum = 0;

		for (int i = 0; i < ITERS; i++)
			sum += PureFinallyHotCaller (i % 10);

		return sum == ExpectedPureFinallySum (ITERS) ? 0 : 1;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	static int ExpectedPureFinallySum (int iters) {
		int sum = 0;
		for (int i = 0; i < iters; i++)
			sum += (i % 10) * 2;
		return sum;
	}
}
