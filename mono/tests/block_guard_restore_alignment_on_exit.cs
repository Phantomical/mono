using System;
using System.Threading;

class Driver {
	static volatile bool foo = false;
	static int res = 1;

	/*
	 * The abort must arrive while the thread is in the finally block, because a
	 * delayed abort is what this test measures. A sleep in Main is not enough. The
	 * thread must compile Func and InnerFunc before it can run them. That compile
	 * takes a large part of the first 100 ms, and all of it on a loaded machine. An
	 * abort that arrives before the try block in Func stops the thread with no
	 * handler, and res keeps its initial value.
	 */
	static ManualResetEvent parked = new ManualResetEvent (false);

	static void InnerFunc () {
		res = 2;
		try {
			res = 3;
		} finally {
			res = 4;
			Console.WriteLine ("EEE");
			parked.Set ();
			while (!foo);
			res = 5;
			Console.WriteLine ("in the finally block");
			Thread.ResetAbort ();
			res = 6;
		}
		res = 7;
		throw new Exception ("lalala");
	}

	static void Func () {
		try {
			InnerFunc ();
		} catch (Exception e) {
			res = 0;
		}
	}

	static int Main () {
		Thread t = new Thread (Func);
		t.Start ();
		parked.WaitOne ();
		t.Abort ();
		foo = true;
		Console.WriteLine ("What now?");
		t.Join ();
		Thread.Sleep (500);
		return res;
	}
}
