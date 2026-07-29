using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// Nested try/catch/finally: three separate clauses in one method.  The x==0 case
// unwinds out of the inner try, through the inner finally, to the outer catch -- one
// throw needing both a cleanup and a handler out of the same frame.  A wrong answer is
// silent rather than a crash: the finally's +10 simply goes missing.
//
// ExpectedNestedHandlers is the oracle and carries NoOptimization, which keeps it on
// the classic JIT.  Without that both sides get compiled the same way and agree on the
// same wrong answer, which is exactly how this defect stayed hidden.

public class NestedHandlers {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (NestedHandlers), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int LeafNestedHandlers (int x) {
		int steps = 0;
		try {
			try {
				if (x == 0)
					throw new InvalidOperationException ();
				steps += 1;
			} finally {
				steps += 10;
			}
			try {
				if (x < 0)
					throw new ArgumentException ();
				steps += 100;
			} catch (ArgumentException) {
				steps += 1000;
			}
		} catch (InvalidOperationException) {
			steps += 10000;
		}
		return steps;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int NestedHandlersHotCaller (int x) {
		return LeafNestedHandlers (x);
	}

	[MethodImpl (MethodImplOptions.NoOptimization | MethodImplOptions.NoInlining)]
	static int ExpectedNestedHandlers (int x) {
		int steps = 0;
		try {
			try {
				if (x == 0)
					throw new InvalidOperationException ();
				steps += 1;
			} finally {
				steps += 10;
			}
			try {
				if (x < 0)
					throw new ArgumentException ();
				steps += 100;
			} catch (ArgumentException) {
				steps += 1000;
			}
		} catch (InvalidOperationException) {
			steps += 10000;
		}
		return steps;
	}

	public static int test_0_nested_handlers () {
		int errors = 0;
		int[] xs = { 0, -1, 5 };
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++) {
			int x = xs [i % xs.Length];
			int r = NestedHandlersHotCaller (x);
			if (r != ExpectedNestedHandlers (x))
				errors += 1;
		}
		return errors;
	}
}
