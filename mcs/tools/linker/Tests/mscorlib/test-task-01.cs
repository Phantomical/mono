using System;
using System.Threading;
using System.Threading.Tasks;

public class C
{
	public static unsafe int Main ()
	{
		TaskScheduler.UnobservedTaskException += (obj, evt) => evt.SetObserved ();
		var mre = new ManualResetEventSlim ();
		var task = Task.Factory.StartNew (() => mre.Wait (200));
		var contFailed = task.ContinueWith (t => {}, TaskContinuationOptions.OnlyOnFaulted);
		var contCanceled = task.ContinueWith (t => {}, TaskContinuationOptions.OnlyOnCanceled);
		var contSuccess = task.ContinueWith (t => {}, TaskContinuationOptions.OnlyOnRanToCompletion);

		mre.Set ();

		if (!contSuccess.Wait (60000))
			return 1;

		return 0;
	}
}
