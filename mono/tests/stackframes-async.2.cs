using System;
using System.Net;
using System.Diagnostics;
using System.Threading;

	class MainClass
	{
		static int frame_count = 0;
		static ManualResetEvent callback_done = new ManualResetEvent (false);

		public static int Main(string[] args)
		{
			AsyncCallback cback = new AsyncCallback(ResolveCallback);
			IAsyncResult res = Dns.BeginGetHostEntry("localhost", cback, null);
			// res is signalled just before the callback is invoked, so waiting on
			// it would still race the walk this test is about.
			if (!callback_done.WaitOne (120000)) {
				Console.WriteLine ("FAILED: the resolve callback never ran (the resolve itself {0})",
						   res.IsCompleted ? "did complete" : "had not completed either");
				return 1;
			}
			if (frame_count < 1) {
				Console.WriteLine ("FAILED: the callback ran but the stack walk found no frames");
				return 1;
			}

			// A test for #444383
			AppDomain.CreateDomain("1").CreateInstance(typeof (Class1).Assembly.GetName ().Name, "Class1");

			return 0;
		}
		
		public static void ResolveCallback(IAsyncResult ar)
		{
		    Console.WriteLine("ResolveCallback()");
		    StackTrace st = new StackTrace();
		    frame_count = st.FrameCount;
	            for(int i = 0; i < st.FrameCount; i++) {
	                StackFrame sf = st.GetFrame(i);
        	        Console.WriteLine("method: {0}", sf.GetMethod());
	            }
        	    Console.WriteLine("ResolveCallback() complete");
        	    callback_done.Set ();
		}
	}

public class Class1
{
	public Class1 () {
		AppDomain.CreateDomain("2").CreateInstance(typeof (Class1).Assembly.GetName ().Name, "Class2");
	}
}

public class Class2
{
	public Class2 () {
		new StackTrace(true);
	}
}
