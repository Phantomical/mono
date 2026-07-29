using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// ThrowingHolder's cctor throws, so its vtable never reaches initialized and
// Compute's class-init preamble runs on every single call.  The exception has to come
// out of Compute and be caught by the caller's handler, both when Compute keeps its
// call and when it is folded in -- where the preamble's call has to be rewritten into
// an invoke on the caller's landing pad.

public class CctorFailure {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (CctorFailure), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static class ThrowingHolder {
		static ThrowingHolder () { throw new InvalidOperationException ("boom"); }

		public static int Compute (int x) {
			int p1 = x + 7, p2 = p1 * 3, p3 = p2 - 2, p4 = p3 ^ 9, p5 = p4 & 0xFF;
			int p6 = p5 | 0x01, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
			int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
			if (p15 == int.MinValue)
				return -1;
			return x + 2;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ThrowingCctorHotCaller (int x) {
		try {
			return ThrowingHolder.Compute (x);
		} catch (TypeInitializationException) {
			return -1;
		}
	}

	public static int test_0_cctor_failure_caught_by_caller () {
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			if (ThrowingCctorHotCaller (i) != -1)
				return 1;
		return 0;
	}
}
