using System;
using System.Threading;

// Statics read in a loop. Without deduplication the class-initialization check
// runs once per iteration, which both costs the loop a call and plants an
// interruption checkpoint in it; the spin below is the shape where that shows.

class Log {
	public static int SignalRuns;
	public static int TableRuns;
}

class Signal {
	public static volatile bool Stop;

	static Signal ()
	{
		Log.SignalRuns++;
	}
}

class Table {
	public static int Step, Base;

	static Table ()
	{
		Log.TableRuns++;
		Step = 3;
		Base = 10;
	}
}

class CctorInitLoop {
	// A counted loop over two statics of one class.
	static long Accumulate (int times)
	{
		long total = 0;

		for (int i = 0; i < times; ++i)
			total += Table.Base + Table.Step * i;
		return total;
	}

	// Spin on a static another thread flips. Bounded so a load hoisted out of
	// the loop fails the test rather than hanging it.
	static bool Spin ()
	{
		long spins = 0;

		while (!Signal.Stop && spins < 4000000000L)
			spins++;
		return Signal.Stop;
	}

	static int Main ()
	{
		long accumulated = Accumulate (1000);
		long expected = 1000L * 10 + 3L * (999L * 1000L / 2);

		if (accumulated != expected) {
			Console.WriteLine ("Accumulate (1000) = {0}, expected {1}",
			                   accumulated, expected);
			return 1;
		}

		if (Log.TableRuns != 1) {
			Console.WriteLine ("Table's cctor ran {0} times, expected 1",
			                   Log.TableRuns);
			return 2;
		}

		Thread setter = new Thread (() => {
			Thread.Sleep (200);
			Signal.Stop = true;
		});

		setter.Start ();

		if (!Spin ()) {
			Console.WriteLine ("the spin gave up before Signal.Stop was set");
			return 3;
		}

		setter.Join ();

		if (Log.SignalRuns != 1) {
			Console.WriteLine ("Signal's cctor ran {0} times, expected 1",
			                   Log.SignalRuns);
			return 4;
		}

		return 0;
	}
}
