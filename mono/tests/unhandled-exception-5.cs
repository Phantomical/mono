using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

class CustomException : Exception
{
}

class Driver
{
	static ManualResetEvent mre = new ManualResetEvent (false);

	class FinalizedClass
	{
		~FinalizedClass ()
		{
			try {
				throw new CustomException ();
			} finally {
				mre.Set ();
			}
		}
	}

	/* expected exit code: 255 */
	static void Main (string[] args)
	{
		if (Environment.GetEnvironmentVariable ("TEST_UNHANDLED_EXCEPTION_HANDLER") != null)
			AppDomain.CurrentDomain.UnhandledException += (s, e) => {};

		new FinalizedClass();

		GC.Collect ();
		GC.WaitForPendingFinalizers ();

		if (!mre.WaitOne (5000))
			Environment.Exit (2);

		/* A correct runtime never gets here at all: the finalizer's exception is
		 * fatal while WaitForPendingFinalizers () above is still running, about
		 * half a second in, so the process is gone before mre is even waited on.
		 * This is only the backstop for a runtime that lets main carry on, and it
		 * is long for the same reason as the rest of the family - a budget that a
		 * loaded machine can outrun is how these tests came to fail at random. */
		Thread.Sleep (30000);

		Environment.Exit (0);
	}
}
