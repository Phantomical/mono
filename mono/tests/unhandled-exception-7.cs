using System;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;
using System.Runtime.Remoting.Messaging;

class CustomException : Exception
{
}

class CrossDomain : MarshalByRefObject
{
	public Action NewDelegateWithTarget ()
	{
		return new Action (Bar);
	}

	public Action NewDelegateWithoutTarget ()
	{
		return () => { throw new CustomException (); };
	}

	public void Bar ()
	{
		throw new CustomException ();
	}
}

class Driver
{
	/* expected exit code: 255 */
	static void Main (string[] args)
	{
		if (Environment.GetEnvironmentVariable ("TEST_UNHANDLED_EXCEPTION_HANDLER") != null)
			AppDomain.CurrentDomain.UnhandledException += (s, e) => {};

		ManualResetEvent mre = new ManualResetEvent (false);

		var ad = AppDomain.CreateDomain ("ad");

		if (Environment.GetEnvironmentVariable ("TEST_UNHANDLED_EXCEPTION_HANDLER") != null)
			ad.UnhandledException += (s, e) => {};

		var cd = (CrossDomain) ad.CreateInstanceAndUnwrap (typeof(CrossDomain).Assembly.FullName, "CrossDomain");

		var action = cd.NewDelegateWithoutTarget ();
		var ares = action.BeginInvoke (Callback, null);

		/* Nothing here can be waited on: the throw happens inside the callback, and
		 * ares is signalled before that callback runs. So this is a backstop rather
		 * than a synchronisation point - reaching Exit means the runtime never
		 * treated the exception as unhandled. Sized to lose to nothing but a real
		 * bug, since the first pass through a cross-domain BeginInvoke has a lot to
		 * compile and five seconds did not always cover it on a loaded machine. */
		Thread.Sleep (30000);

		Environment.Exit (1);
	}

	static void Callback (IAsyncResult iares)
	{
		((Action) ((AsyncResult) iares).AsyncDelegate).EndInvoke (iares);
	}
}
