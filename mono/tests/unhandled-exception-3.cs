using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

class CustomException : Exception
{
}

class Driver
{
	/* expected exit code: 255 */
	static void Main (string[] args)
	{
		if (Environment.GetEnvironmentVariable ("TEST_UNHANDLED_EXCEPTION_HANDLER") != null)
			AppDomain.CurrentDomain.UnhandledException += (s, e) => {};

		ManualResetEvent mre = new ManualResetEvent (false);

		ThreadPool.QueueUserWorkItem (_ => { try { throw new CustomException (); } finally { mre.Set (); } });

		if (!mre.WaitOne (5000))
			Environment.Exit (2);

		/* The finally that set mre runs while the exception is still propagating, so
		 * the threadpool thread has not reached the unhandled handler yet. A correct
		 * runtime takes the process down in the time it takes to unwind one frame;
		 * this is only the backstop that turns "it never happened" into a failure,
		 * so it is sized to lose to nothing but a real bug. A second was short
		 * enough that a loaded machine reached Exit (0) first. */
		Thread.Sleep (30000);

		Environment.Exit (0);
	}
}
