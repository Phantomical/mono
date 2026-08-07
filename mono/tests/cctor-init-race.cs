using System;
using System.Threading;

// Two threads reaching a class initializer at once. One runs it and the other
// blocks until it is done; both then see the initialized values, and neither
// may see the half-built state the initializer passes through.
//
// The thread that ran the initializer is the interesting one: its vtable does
// not read as initialized even after it has finished, so a check deleted on its
// behalf has to have been deleted because another check dominated it, not
// because the class looked ready.

class Log {
	public static int SharedRuns;
}

class Shared {
	public static int First, Second, Third;

	static Shared ()
	{
		Interlocked.Increment (ref Log.SharedRuns);
		First = 1;
		Thread.Sleep (150);
		Second = 2;
		Third = 3;
	}
}

class CctorInitRace {
	static volatile int failures;
	static int reads;

	// Several accesses in one method, so all but the first ride on the first
	// one's check - on both the initializing thread and the blocked one.
	static void Read ()
	{
		int first = Shared.First;
		int second = Shared.Second;
		int third = Shared.Third;

		if (first != 1 || second != 2 || third != 3) {
			Console.WriteLine ("saw {0}, {1}, {2}; expected 1, 2, 3",
			                   first, second, third);
			Interlocked.Increment (ref failures);
		}

		Interlocked.Increment (ref reads);
	}

	static int Main ()
	{
		Thread[] threads = new Thread[4];

		for (int i = 0; i < threads.Length; ++i) {
			threads[i] = new Thread (() => {
				for (int n = 0; n < 20; ++n)
					Read ();
			});
		}

		foreach (Thread t in threads)
			t.Start ();
		foreach (Thread t in threads)
			t.Join ();

		if (failures != 0) {
			Console.WriteLine ("{0} threads saw a half-built class", failures);
			return 1;
		}

		if (reads != threads.Length * 20) {
			Console.WriteLine ("{0} reads, expected {1}", reads,
			                   threads.Length * 20);
			return 2;
		}

		if (Log.SharedRuns != 1) {
			Console.WriteLine ("Shared's cctor ran {0} times, expected 1",
			                   Log.SharedRuns);
			return 3;
		}

		return 0;
	}
}
