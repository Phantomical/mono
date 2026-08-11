using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

class CustomException : Exception
{
}

class Driver
{
	/* Expected exit code: 1 */
	static void Main (string[] args)
	{
		if (Environment.GetEnvironmentVariable ("TEST_UNHANDLED_EXCEPTION_HANDLER") != null)
			AppDomain.CurrentDomain.UnhandledException += (s, e) => {};

		ManualResetEvent mre = new ManualResetEvent (false);

		var t = new Thread (new ThreadStart (() => { try { throw new CustomException (); } finally { mre.Set (); } }));
		t.Start ();

		if (!mre.WaitOne (5000))
			Environment.Exit (2);

		/* The finally that set mre runs while the exception is still propagating, so
		 * the thread has not reached the unhandled handler yet and mre says nothing
		 * about whether the runtime has dealt with it. Join () is the wait that does:
		 * the thread is not finished until the unhandled handler on it has run, and
		 * on a correct runtime that takes the process down and this never returns.
		 * Reaching Exit (0) therefore means the exception went unnoticed. */
		t.Join ();

		Environment.Exit (0);
	}
}
