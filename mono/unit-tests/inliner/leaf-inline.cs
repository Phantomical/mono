using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A leaf the inliner should fold, checked by a side-effect counter as well as by
// the value: a double-run or dropped-run bug is what pure arithmetic can paper
// over.

public class LeafInline {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (LeafInline), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int leaf_square_plus_calls;

	static int LeafSquarePlus (int x) {
		leaf_square_plus_calls++;
		int p1 = x + 1, p2 = p1 * 3, p3 = p2 - 7, p4 = p3 ^ 11, p5 = p4 & 0xFF;
		int p6 = p5 | 0x20, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;			// unreachable for the x range this is called with
		return x * x + 3;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int HotLeafCaller (int x) {
		return LeafSquarePlus (x) + 1;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_leaf_inline_correct () {
		leaf_square_plus_calls = 0;
		long sum = 0;
		const int CYCLE = 50;
		int ITERS = (Iters () + CYCLE - 1) / CYCLE * CYCLE;
		for (int i = 0; i < ITERS; i++)
			sum += HotLeafCaller (i % CYCLE);
		long perCycle = 0;
		for (int k = 0; k < CYCLE; k++)
			perCycle += k * k + 3 + 1;
		long expected = perCycle * (ITERS / CYCLE);
		if (sum != expected)
			return 1;
		if (leaf_square_plus_calls != ITERS)
			return 2;			// folded body ran a different number of times
						// than it was called - exactly-once semantics broke.
		return 0;
	}
}
