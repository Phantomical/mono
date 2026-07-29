using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// The same trigger on a generic class.  This shape used to be unreachable:
// pending_class_init_vtable () gives up on a class whose vtable it cannot pin down,
// and before the callee was compiled specialized there was no single vtable to name.
// A closed instantiation has exactly one, so the preamble can trigger it like any
// other.

public class GenericClassInitTrigger {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (GenericClassInitTrigger), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static class GenericTriggerObserver {
		public static int Ran;
	}

	static class GenericTriggerHolder<T> {
		static GenericTriggerHolder () { GenericTriggerObserver.Ran += 1; }

		public static int Compute (int x) {
			int p1 = x + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
			int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 + 1;
			int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
			if (p15 == int.MinValue)
				return -1;
			return x + 11;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenericComputeHotCaller (int x) {
		// Two instantiations, so two distinct classes each owing a cctor: the
		// reference-type one is what mono would share, the value-type one it would
		// not, and both must end up initialized exactly once.
		return GenericTriggerHolder<string>.Compute (x) + GenericTriggerHolder<int>.Compute (x);
	}

	// INLINER-EXPECT: folded GenericClassInitTrigger/GenericTriggerHolder`1<string>:Compute (int)
	// INLINER-EXPECT: folded GenericClassInitTrigger/GenericTriggerHolder`1<int>:Compute (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_cctor_class_init_trigger () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += GenericComputeHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += (i + 11) * 2;
		if (sum != expected)
			return 1;
		// One cctor run per instantiation, and no more: a preamble that re-ran it
		// would show up here just as loudly as one that never ran it.
		return GenericTriggerObserver.Ran == 2 ? 0 : 2;
	}
}
