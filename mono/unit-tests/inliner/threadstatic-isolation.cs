using System;
using System.Runtime.CompilerServices;
using System.Threading;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// [ThreadStatic] isolation: each thread's running total has to match what it alone
// contributed, so a folded access that lost the per-thread offset shows up as another
// thread's value.

public class ThreadstaticIsolation {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (ThreadstaticIsolation), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static class TSHolder {
		[ThreadStatic]
		public static int Value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int TSHotCaller (int x) {
		TSHolder.Value += x;
		return TSHolder.Value;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_threadstatic_isolation () {
		const int NUM_THREADS = 4;
		int ITERS = Iters ();
		bool[] ok = new bool[NUM_THREADS];
		var threads = new Thread[NUM_THREADS];
		for (int t = 0; t < NUM_THREADS; t++) {
			int idx = t;
			threads [t] = new Thread (() => {
				int inc = idx + 1;
				int expected = 0, last = 0;
				for (int i = 0; i < ITERS; i++) {
					expected += inc;
					last = TSHotCaller (inc);
				}
				ok [idx] = (last == expected) && (TSHolder.Value == expected);
			});
		}
		foreach (var th in threads)
			th.Start ();
		foreach (var th in threads)
			th.Join ();
		foreach (bool o in ok)
			if (!o)
				return 1;
		return 0;
	}
}
