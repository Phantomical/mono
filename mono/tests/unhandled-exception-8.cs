
using System;
using System.Threading;

class CustomException : Exception
{
}

class Driver
{
	/* expected exit code: 255 */
	public static void Main ()
	{
		if (Environment.GetEnvironmentVariable ("TEST_UNHANDLED_EXCEPTION_HANDLER") != null)
			AppDomain.CurrentDomain.UnhandledException += (s, e) => {};

		ManualResetEvent mre = new ManualResetEvent(false);

		ThreadPool.RegisterWaitForSingleObject (mre, (state, timedOut) => { throw new CustomException (); }, null, -1, true);
		mre.Set();

		/* The registered wait runs the throw on a threadpool thread with nothing to
		 * wait on, so this is a backstop rather than a synchronisation point:
		 * returning from Main means the runtime never treated the exception as
		 * unhandled. Sized to lose to nothing but a real bug: this is the same shape
		 * as unhandled-exception-7, which did lose its five seconds under load. */
		Thread.Sleep (30000);
	}
}