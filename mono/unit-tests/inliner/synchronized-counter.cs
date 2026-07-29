using System;
using System.Runtime.CompilerServices;
using System.Threading;

// materialize_callee () refuses a METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED callee
// unconditionally (mono-side, no trace line) because its monitor enter/exit lives in
// the synchronized wrapper, not the raw body.  Several threads hammer the same
// counter; an exact final count is only possible if every increment stayed serialized
// by the lock, which a dropped monitor would very quickly break.

public class SynchronizedCounter {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (SynchronizedCounter), args);
	}

	static class SyncCounter {
		static int count;

		[MethodImpl (MethodImplOptions.Synchronized)]
		public static void Increment () {
			count++;
		}

		public static int Count { get { return count; } }

		// --regression re-runs every test_N in the same process, once per
		// optimization combination, so static state must be reset per call.
		public static void Reset () { count = 0; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void SyncHotCaller () {
		SyncCounter.Increment ();
	}

	public static int test_0_synchronized_counter_contention () {
		SyncCounter.Reset ();
		const int NUM_THREADS = 8;
		const int ITERS = 5000;
		var threads = new Thread[NUM_THREADS];
		for (int t = 0; t < NUM_THREADS; t++) {
			threads [t] = new Thread (() => {
				for (int i = 0; i < ITERS; i++)
					SyncHotCaller ();
			});
		}
		foreach (var th in threads)
			th.Start ();
		foreach (var th in threads)
			th.Join ();
		return SyncCounter.Count == NUM_THREADS * ITERS ? 0 : 1;
	}
}
