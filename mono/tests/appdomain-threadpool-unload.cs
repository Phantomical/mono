
using System;
using System.Linq;
using System.Threading;

class Driver
{
	class ThreadPoolLauncherObject
	{
		public volatile int i = 0;

		public ThreadPoolLauncherObject ()
		{
			ThreadPool.QueueUserWorkItem (_ => { for (int i = 0; i < 10 * 1000 * 1000; ++i); }, null);
		}
	}

	public static void Main ()
	{
		int count = 0;
		object o = new object ();

		foreach (var i in
			Enumerable.Range (0, 100)
				.AsParallel ().WithDegreeOfParallelism (Environment.ProcessorCount)
				.Select (i => {
					AppDomain ad;

					ad = AppDomain.CreateDomain ("testdomain" + i);
					ad.CreateInstance (typeof (ThreadPoolLauncherObject).Assembly.FullName, typeof (ThreadPoolLauncherObject).FullName);

					Thread.Sleep (10);

					// Unload bounds its wait for ad's queued work with a timeout
					// (MONO_DOMAIN_UNLOAD_TIMEOUT, default 1s) and throws once it
					// expires instead of hanging. The queued busy loop above can
					// still hit that timeout while interpreted, so this is an
					// accepted outcome here, not a failure.
					try {
						AppDomain.Unload (ad);
					} catch (CannotUnloadAppDomainException) {
					}

					return i;
				})
				.Select (i => {
					lock (o) {
						count += 1;

						Console.Write (".");
						if (count % 25 == 0)
							Console.WriteLine ();
					}

					return i;
				})
		) {
		}
	}
}
