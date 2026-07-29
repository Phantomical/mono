using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// The mirror of the cctor-barrier fixtures: the callee touches no static state, so
// nothing about its body is suspect.  What is at stake is that the CALL is the
// class-init trigger for the callee's own class -- the first one lands in a
// trampoline, which compiles the method and then runs its cctor.  Fold the callee in
// and the only surviving trigger is the class-init preamble on the tier-1 body; if
// that is missing the cctor never runs for the whole process, and the failure
// surfaces at an arbitrary static read far away.
//
// Only bites when the caller promotes before the callee's class is initialized, i.e.
// at MONO_TIERED_CALL_THRESHOLD=0.  Compute does no division and reads nothing
// static, so the trigger is the only thing left to blame; no IL padding is needed
// either, because classic mini declines a callee whose class is not initialized yet.

public class ClassInitTrigger {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (ClassInitTrigger), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static class TriggerObserver {
		public static int Ran;
	}

	static class TriggerHolder {
		static TriggerHolder () { TriggerObserver.Ran = 1234; }

		public static int Compute (int x) {
			int p1 = x + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
			int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 + 1;
			int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
			if (p15 == int.MinValue)
				return -1;
			return x + 9;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ComputeHotCaller (int x) {
		return TriggerHolder.Compute (x);
	}

	// INLINER-EXPECT: folded ClassInitTrigger/TriggerHolder:Compute (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_class_init_trigger () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ComputeHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 9;
		if (sum != expected)
			return 1;
		return TriggerObserver.Ran == 1234 ? 0 : 2;
	}
}
