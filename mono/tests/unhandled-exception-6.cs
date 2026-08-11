using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using System.Runtime.Remoting.Messaging;

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

		var action = new Action (Delegate);
		var ares = action.BeginInvoke (Callback, null);

		/* Nothing here can be waited on: the throw happens inside the callback, and
		 * ares is signalled before that callback runs. So this is a backstop rather
		 * than a synchronisation point - reaching Exit means the runtime never
		 * treated the exception as unhandled. Sized to lose to nothing but a real
		 * bug: this is the same shape as unhandled-exception-7, which did lose its
		 * five seconds under load. */
		Thread.Sleep (30000);

		Environment.Exit (1);
	}

	static void Delegate ()
	{
		throw new CustomException ();
	}

	static void Callback (IAsyncResult iares)
	{
		((Action) ((AsyncResult) iares).AsyncDelegate).EndInvoke (iares);
	}
}
