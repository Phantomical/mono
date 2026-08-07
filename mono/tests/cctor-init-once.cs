using System;

// A class initializer runs exactly once however many accesses ask for it, and
// every access still sees the initialized values. The backend emits a check at
// each access and deletes the ones a dominating check covers, so the count here
// is what says the survivors are in the right places.

class Log {
	public static int CountedRuns;
	public static int SecondRuns;
}

class Counted {
	public static int A, B;

	static Counted ()
	{
		Log.CountedRuns++;
		A = 11;
		B = 22;
	}
}

class Second {
	public static int C;

	static Second ()
	{
		Log.SecondRuns++;
		C = 5;
	}
}

class CctorInitOnce {
	// Several accesses to one class in one method: only the first needs a check.
	static int Straight ()
	{
		return Counted.A + Counted.B + Counted.A + Counted.B;
	}

	// The same in a loop, where an undeduplicated check runs per iteration.
	static int Loop (int times)
	{
		int total = 0;

		for (int i = 0; i < times; ++i)
			total += Counted.A + Counted.B;
		return total;
	}

	// Two classes interleaved: each needs its own first check.
	static int Interleaved ()
	{
		return Counted.A + Second.C + Counted.B + Second.C;
	}

	static int Main ()
	{
		int straight = Straight ();

		if (straight != 66) {
			Console.WriteLine ("Straight () = {0}, expected 66", straight);
			return 1;
		}

		int loop = Loop (100);

		if (loop != 3300) {
			Console.WriteLine ("Loop (100) = {0}, expected 3300", loop);
			return 2;
		}

		int interleaved = Interleaved ();

		if (interleaved != 43) {
			Console.WriteLine ("Interleaved () = {0}, expected 43", interleaved);
			return 3;
		}

		if (Log.CountedRuns != 1) {
			Console.WriteLine ("Counted's cctor ran {0} times, expected 1",
			                   Log.CountedRuns);
			return 4;
		}

		if (Log.SecondRuns != 1) {
			Console.WriteLine ("Second's cctor ran {0} times, expected 1",
			                   Log.SecondRuns);
			return 5;
		}

		return 0;
	}
}
