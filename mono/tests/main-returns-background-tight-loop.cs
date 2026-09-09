using System;
using System.Threading;

/*
Regression test for a shutdown hang. A background thread stuck in a tight,
call-free allocation loop could survive mono_thread_manage_internal ()'s
abort phase forever. A suspend signal that catches it outside managed code
arms an interrupt token nothing later consumes. Domain churn on the main
thread keeps the background threads crossing into and out of native
allocation code. That crossing is what gives the signal a chance to land
badly.
*/
class MainReturnsBackgroundTightLoop {
	static void AllocStuff ()
	{
		var x = new object ();
		for (int i = 0; i < 300; ++i)
			x = new byte [i];
	}

	static void BackgroundNoise ()
	{
		while (true)
			AllocStuff ();
	}

	static void Main ()
	{
		for (int i = 0; i < Math.Max (1, Environment.ProcessorCount / 2); ++i) {
			var t = new Thread (BackgroundNoise);
			t.IsBackground = true;
			t.Start ();
		}

		var start = DateTime.UtcNow;
		while ((DateTime.UtcNow - start).TotalSeconds < 5) {
			var ad = AppDomain.CreateDomain ("domain_" + Guid.NewGuid ());
			ad.DoCallBack (new CrossAppDomainDelegate (AllocStuff));
			AppDomain.Unload (ad);
		}
		Console.WriteLine ("main returns");
	}
}
