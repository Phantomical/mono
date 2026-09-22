// Exercise the race between a thread resetting its abort and another thread
// posting a new one. A failure is a process abort or hang, not a managed error.

using System;
using System.Threading;

class Driver
{
	const int rounds = 100;
	const int aborts_per_round = 300;

	static volatile bool stop;

	static void Victim ()
	{
		while (!stop) {
			try {
				Thread.ResetAbort ();
			} catch (ThreadStateException) {
				// No abort was pending.
			} catch (ThreadAbortException) {
				try {
					Thread.ResetAbort ();
				} catch (Exception) {
				}
			}
		}
	}

	public static int Main ()
	{
		for (int round = 0; round < rounds; ++round) {
			stop = false;

			Thread victim = new Thread (Victim);
			victim.IsBackground = true;
			victim.Start ();

			// Replace a victim that dies; its unhandled-exception cleanup also
			// exercises the reset path.
			for (int i = 0; i < aborts_per_round && victim.IsAlive; ++i) {
				victim.Abort ();
				Thread.Sleep (1);
			}

			stop = true;
			victim.Join (2000);
		}

		Console.WriteLine ("ok");
		return 0;
	}
}
