using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// The call-site half of the EH rule.  TryFinallyHotCaller needs its own try/catch to
// swallow LeafTryFinally's exception, so the call compiles to an `invoke` -- it has an
// unwind edge to this frame's handler.  Folding a clause-bearing body into a site like
// that would nest the two methods' clauses, and the pads the callee brings carry
// selector switches that know nothing of the caller's, so the site keeps its
// trampoline call.  This being the only caller, nothing names the materialized body
// and it is dropped again.

public class TryFinallyLeaf {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (TryFinallyLeaf), args);
	}

	static int finally_run_count;

	static int LeafTryFinally (int x) {
		try {
			if (x < 0)
				throw new ArgumentException ();
			return x * 3;
		} finally {
			finally_run_count++;
		}
	}

	// INLINER-EXPECT: refused TryFinallyLeaf:LeafTryFinally (int)
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int TryFinallyHotCaller (int x) {
		try {
			return LeafTryFinally (x);
		} catch (ArgumentException) {
			return -1;
		}
	}

	// The expected value IS the finally-run count: every call runs the finally exactly
	// once, whether it returns normally or throws through it, so after ITERS calls the
	// count must be exactly ITERS.  A dropped finally - the silent miscompile this gate
	// exists to prevent - surfaces here as a wrong, smaller number.
	public static int test_5000_try_finally_run_count_refused () {
		const int ITERS = 5000;
		finally_run_count = 0;
		int caught = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = (i % 5 == 0) ? -1 : (i % 100);
			int r = TryFinallyHotCaller (x);
			if (x >= 0) {
				if (r != x * 3)
					return -1;
			} else {
				if (r != -1)
					return -2;
				caught++;
			}
		}
		if (caught == 0)
			return -3;			// throwing branch never exercised - test is broken.
		return finally_run_count;
	}
}
