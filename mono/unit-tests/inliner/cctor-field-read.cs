using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// An otherwise-eligible leaf that reads a static field guarded by a real cctor.
// SeedHolder has an explicit static ctor, so the front end leaves a barrier in the
// callee's own body and folding it in has to carry that barrier along.

public class CctorFieldRead {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (CctorFieldRead), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static class SeedHolder {
		public static int Seed;
		static SeedHolder () { Seed = 424242; }
	}

	static int ReadSeed (int x) {
		int p1 = x + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
		int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 / 3 + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x + SeedHolder.Seed;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadSeedHotCaller (int x) {
		return ReadSeed (x);
	}

	// INLINER-EXPECT: folded CctorFieldRead:ReadSeed (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_field_read () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReadSeedHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 424242;
		return sum == expected ? 0 : 1;
	}
}
