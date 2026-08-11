using System;
using Mono.Tasklets;

// A continuation is a copy of the native stack between Store () and the frame
// that called Mark (), so it only means anything when every frame in that
// region has one. MONO_TEST_TASKLETS says which of the two outcomes the
// configuration this runs under must produce.
class Tasklets
{
	static int total;

	static int Main ()
	{
		bool refusal = Environment.GetEnvironmentVariable ("MONO_TEST_TASKLETS") == "refuse";
		Continuation cont = new Continuation ();

		try {
			cont.Mark ();
		} catch (NotSupportedException e) {
			Console.WriteLine ("refused: {0}", e.Message);
			if (refusal)
				return 0;
			Console.WriteLine ("FAILED: continuations should work here");
			return 1;
		}

		if (refusal) {
			Console.WriteLine ("FAILED: Mark () should have refused");
			return 1;
		}

		// Store () returns 0 the first time and whatever Restore () was
		// passed on every later arrival, with the rest of the frame - value
		// and the loop's sum - put back the way it was.
		int value = 0;
		int ret = cont.Store (0);

		for (int i = ret; i < 10; i++)
			value += i;

		if (value > 0) {
			total += value;
			cont.Restore (ret + 1);
		}

		if (total != 330) {
			Console.WriteLine ("FAILED: total is {0}, want 330", total);
			return 1;
		}

		return 0;
	}
}
