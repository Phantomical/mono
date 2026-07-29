using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// Three independent leaf calls composed at the root: more than one candidate per
// root, and their results still compose once folded together.

public class ChainedLeaf {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (ChainedLeaf), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int LeafChainA (int x) {
		int p1 = x + 2, p2 = p1 * 5, p3 = p2 - 9, p4 = p3 ^ 3, p5 = p4 & 0xFF;
		int p6 = p5 | 0x40, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 2) + 1;
		int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x + 1;
	}

	static int LeafChainB (int x) {
		int p1 = x + 3, p2 = p1 * 7, p3 = p2 - 4, p4 = p3 ^ 6, p5 = p4 & 0xFF;
		int p6 = p5 | 0x08, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 3) + 1;
		int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x * 2;
	}

	static int LeafChainC (int x) {
		int p1 = x + 5, p2 = p1 * 2, p3 = p2 - 6, p4 = p3 ^ 9, p5 = p4 & 0xFF;
		int p6 = p5 | 0x02, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x - 3;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ChainedHotCaller (int x) {
		return LeafChainC (LeafChainB (LeafChainA (x)));
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_chained_leaf_inline () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ChainedHotCaller (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = i % 100;
			expected += ((x + 1) * 2) - 3;
		}
		return sum == expected ? 0 : 1;
	}
}
