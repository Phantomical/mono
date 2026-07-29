using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// The caller branches on the leaf's return value, so a fold that swapped an
// argument or dropped a term sends execution down the wrong branch rather than
// just computing a wrong number in a straight line.

public class LeafBranch {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (LeafBranch), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int LeafCompute (int x) {
		int p1 = x + 4, p2 = p1 * 3, p3 = p2 - 2, p4 = p3 ^ 8, p5 = p4 & 0xFF;
		int p6 = p5 | 0x01, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return (x * x) & 7;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int BranchHotCaller (int x) {
		int v = LeafCompute (x);
		return v > 3 ? v * 10 : v + 1;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_leaf_computed_value_branch () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += BranchHotCaller (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int v = ((i % 100) * (i % 100)) & 7;
			expected += v > 3 ? v * 10 : v + 1;
		}
		return sum == expected ? 0 : 1;
	}
}
