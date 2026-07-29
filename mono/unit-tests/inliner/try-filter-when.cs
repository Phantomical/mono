using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A filter (`when`) clause.  Custom-emit EH's allowlist is {catch, finally, fault}, so
// a filter fails materialize_callee's own front-end compile outright and the callee is
// never even offered to the leaf/EH gates -- it just stays a trampoline call.  No trace
// line to look for, only correctness.

public class TryFilterWhen {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (TryFilterWhen), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int LeafWithFilter (int x) {
		try {
			if (x == 0)
				throw new InvalidOperationException ("zero");
			return x * 2;
		} catch (InvalidOperationException ex) when (ex.Message == "zero") {
			return -1;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FilterHotCaller (int x) {
		return LeafWithFilter (x);
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_try_filter_when_refused () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += FilterHotCaller (i % 50);
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = i % 50;
			expected += x == 0 ? -1 : x * 2;
		}
		return sum == expected ? 0 : 1;
	}
}
