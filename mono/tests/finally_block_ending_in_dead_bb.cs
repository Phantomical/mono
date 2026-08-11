using System;
using System.Threading;

class Driver {
	static volatile bool foo = false;
	static int res = 1;
	static ManualResetEvent in_finally = new ManualResetEvent (false);

	static void Stuff () {
		res = 2;
		try {
			res = 3;
		} finally {
			res = 4;
			in_finally.Set ();
			while (!foo);
			Thread.ResetAbort ();
			res = 0;
		}
	}

	static int Main () {
		Thread t = new Thread (Stuff);
		t.Start ();
		// The abort has to arrive while the thread is inside the finally, so wait
		// for it to say that it is. Sleeping for a fixed time instead is a bet on
		// how long a thread takes to start and reach the block, and a loaded
		// machine loses it: the abort lands before Stuff has run at all, and the
		// test reports a final state of 1.
		in_finally.WaitOne ();
		t.Abort ();
		foo = true;
		t.Join ();
		if (res != 0)
			Console.WriteLine ("Could not abort thread final state {0}", res);
		return res;
	}
}