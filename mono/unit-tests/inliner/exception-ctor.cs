using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// Mono attaches a stack trace to an exception when it is THROWN, not when it is
// constructed, so a freshly-`new`ed exception must report a null StackTrace whichever
// tier built the frame it was constructed in.  The constructor call makes this non-leaf.

public class ExceptionCtor {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (ExceptionCtor), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int LeafExceptionCtorNoStackYet (int x) {
		var ex = new InvalidOperationException ("x=" + x);
		return ex.StackTrace == null ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ExceptionCtorHotCaller (int x) {
		return LeafExceptionCtorNoStackYet (x);
	}

	public static int test_0_exception_ctor_no_stack_captured_yet () {
		int sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ExceptionCtorHotCaller (i);
		return sum == ITERS ? 0 : 1;
	}
}
