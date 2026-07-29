using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// An elided foreign barrier -- the shape with nothing in the IR to notice.
// LazyHolder has no explicit static ctor, so the C# compiler marks it
// beforefieldinit and the front end emits no barrier inside ReadLazy at all; it
// leaves that cctor to SFLDA patch resolution, which a tier-1 compile skips
// (run_cctors = FALSE).  So the guard has to come from the metadata scan
// (collect_static_access_classes ()) as a class-init preamble on the materialized
// body.  Fold ReadLazy in without one and Value reads back as 0.

public class BeforefieldinitStatic {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (BeforefieldinitStatic), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static class LazyHolder {
		public static int Value = MakeValue ();
		static int MakeValue () { LazyObserver.Ran = 1; return 31337; }
	}

	static class LazyObserver {
		public static int Ran;
	}

	static int ReadLazy (int x) {
		int p1 = x + 4, p2 = p1 * 3, p3 = p2 - 8, p4 = p3 ^ 12, p5 = p4 & 0xFF;
		int p6 = p5 | 0x04, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x + LazyHolder.Value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadLazyHotCaller (int x) {
		return ReadLazy (x);
	}

	// INLINER-EXPECT: folded BeforefieldinitStatic:ReadLazy (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_beforefieldinit_static () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReadLazyHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 31337;
		if (sum != expected)
			return 1;
		// A guard that never ran leaves the cctor unrun for the whole process, which
		// the value check only catches because Value happens to be non-zero.
		return LazyObserver.Ran == 1 ? 0 : 2;
	}
}
