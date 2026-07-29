using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// The same elided-barrier shape with two foreign classes in one callee: the
// preamble has to chain a guard per class rather than emit one and stop.

public class TwoBeforefieldinitStatics {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (TwoBeforefieldinitStatics), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static class PairObserver {
		public static int Left, Right;
	}

	static class PairLeft {
		public static int Value = MakeValue ();
		static int MakeValue () { PairObserver.Left = 1; return 100; }
	}

	static class PairRight {
		public static int Value = MakeValue ();
		static int MakeValue () { PairObserver.Right = 1; return 7; }
	}

	static int ReadPair (int x) {
		int p1 = x + 3, p2 = p1 * 5, p3 = p2 - 7, p4 = p3 ^ 9, p5 = p4 & 0xFF;
		int p6 = p5 | 0x02, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x + PairLeft.Value + PairRight.Value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadPairHotCaller (int x) {
		return ReadPair (x);
	}

	// INLINER-EXPECT: folded TwoBeforefieldinitStatics:ReadPair (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_two_beforefieldinit_statics () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReadPairHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 107;
		if (sum != expected)
			return 1;
		return (PairObserver.Left == 1 && PairObserver.Right == 1) ? 0 : 2;
	}
}
