using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// The barrier belongs where the access is, not in the callee's prologue.  ColdHolder
// is read only down a branch the loop never takes, so its cctor must never run -- a
// preamble guard would run it on the first call and ColdObserver would say so.
// Explicit static ctor on purpose: a beforefieldinit class is initialized eagerly
// when the accessing method is JITted, which is tier-dependent and would make this
// untestable.

public class CctorColdPath {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (CctorColdPath), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static class ColdObserver {
		public static int Ran;
	}

	static class ColdHolder {
		public static int Value;
		static ColdHolder () { ColdObserver.Ran = 1; Value = 99; }
	}

	static int ReadCold (int x) {
		int p1 = x + 6, p2 = p1 * 7, p3 = p2 - 2, p4 = p3 ^ 3, p5 = p4 & 0xFF;
		int p6 = p5 | 0x20, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return ColdHolder.Value;
		return x + 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadColdHotCaller (int x) {
		return ReadCold (x);
	}

	// INLINER-EXPECT: folded CctorColdPath:ReadCold (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_barrier_stays_on_its_path () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReadColdHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 1;
		if (sum != expected)
			return 1;
		return ColdObserver.Ran == 0 ? 0 : 2;
	}
}
