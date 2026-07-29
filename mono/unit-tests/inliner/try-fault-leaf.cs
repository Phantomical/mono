using System;
using System.Runtime.CompilerServices;

using MonoTests.Inliner;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// try/fault, which C# has no syntax for -- the callee lives in inliner-fault.il.  The
// fault handler must run exactly on the exceptional path and never on the normal one.
// Like LeafTryFinally, TryFaultHotCaller's own try/catch turns this call into an
// invoke, so it is excluded by the S0 invoke guard rather than ever reaching the
// personality-function check.

public class TryFaultLeaf {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (TryFaultLeaf), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int TryFaultHotCaller (int x) {
		try {
			return FaultHelpers.TryFault (x);
		} catch (InvalidOperationException) {
			return -1;
		}
	}

	public static int test_0_try_fault_leaf_refused () {
		FaultHelpers.FaultRunCount = 0;
		int ITERS = Iters ();
		int caught = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = (i % 4 == 0) ? -1 : (i % 50);
			int r = TryFaultHotCaller (x);
			if (x >= 0) {
				if (r != x * 2)
					return 1;
			} else {
				if (r != -1)
					return 2;
				caught++;
			}
		}
		if (caught == 0)
			return 3;
		if (FaultHelpers.FaultRunCount != caught)
			return 4;			// fault ran a different number of times than
					// the exceptional path was taken.
		return 0;
	}
}
