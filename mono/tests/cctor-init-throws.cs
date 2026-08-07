using System;

// A class initializer that throws. The first access reports it as a
// TypeInitializationException and so does every access after, without the
// initializer running again - so an access whose check was deleted as redundant
// must still be one the surviving check reached.

class Log {
	public static int BoomRuns;
}

class Boom {
	public static int V = 3;

	static Boom ()
	{
		Log.BoomRuns++;
		throw new InvalidOperationException ("boom");
	}
}

class CctorInitThrows {
	// Reads the same failing class three times over. The first throws; the
	// two behind it are never reached, which is exactly why deleting their
	// checks is sound.
	static int Repeated ()
	{
		return Boom.V + Boom.V + Boom.V;
	}

	static bool ThrowsTypeInit (Func<int> body)
	{
		try {
			body ();
			return false;
		} catch (TypeInitializationException e) {
			return e.InnerException is InvalidOperationException;
		}
	}

	static int Main ()
	{
		if (!ThrowsTypeInit (Repeated)) {
			Console.WriteLine ("first access did not report the failed cctor");
			return 1;
		}

		if (Log.BoomRuns != 1) {
			Console.WriteLine ("Boom's cctor ran {0} times, expected 1",
			                   Log.BoomRuns);
			return 2;
		}

		// A class left in the failed state stays failed, however often it is
		// asked for and from wherever.
		for (int i = 0; i < 5; ++i) {
			if (!ThrowsTypeInit (Repeated)) {
				Console.WriteLine ("access {0} did not throw", i);
				return 3;
			}
			if (!ThrowsTypeInit (() => Boom.V)) {
				Console.WriteLine ("lambda access {0} did not throw", i);
				return 4;
			}
		}

		if (Log.BoomRuns != 1) {
			Console.WriteLine ("Boom's cctor re-ran, {0} times in total",
			                   Log.BoomRuns);
			return 5;
		}

		return 0;
	}
}
