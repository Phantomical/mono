using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// The call site uses the vararg calling convention, so this exercises that the pass
// (and the translator's call codegen under it) handles a non-ordinary call shape
// without miscompiling it -- whether or not it is ever a candidate at all.

public class Varargs {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (Varargs), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SumVarArgs (__arglist) {
		var ai = new ArgIterator (__arglist);
		int sum = 0;
		int n = ai.GetRemainingCount ();
		for (int i = 0; i < n; i++) {
			TypedReference tr = ai.GetNextArg ();
			sum += __refvalue (tr, int);
		}
		return sum;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int VarArgsHotCaller (int a, int b, int c) {
		return SumVarArgs (__arglist (a, b, c));
	}

	public static int test_0_varargs_sum () {
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++) {
			int a = i % 7, b = i % 11, c = i % 13;
			if (VarArgsHotCaller (a, b, c) != a + b + c)
				return 1;
		}
		return 0;
	}
}
