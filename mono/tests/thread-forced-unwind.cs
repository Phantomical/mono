using System;
using System.Runtime.InteropServices;
using System.Threading;

/*
 * pthread_exit is a forced unwind: glibc walks every frame below it with
 * _Unwind_ForcedUnwind and calls the personality routine each one names. A
 * JIT'd method with an exception clause names one, so the try/finally here is
 * what puts the backend's routine on that path.
 *
 * Nothing may run out of it - a forced unwind runs no handlers - so the finally
 * must not fire, and the process has to survive to report the exit code Main
 * asks for.
 */
class Driver
{
	[DllImport ("libc")]
	extern static void pthread_exit (IntPtr value);

	static int ran_finally;
	static int ran_catch;

	static void Body ()
	{
		try {
			try {
				pthread_exit (IntPtr.Zero);
			} catch (Exception) {
				Interlocked.Increment (ref ran_catch);
			}
		} finally {
			Interlocked.Increment (ref ran_finally);
		}
	}

	static int Main ()
	{
		Thread t = new Thread (Body);
		t.Start ();
		t.Join ();

		if (Volatile.Read (ref ran_catch) != 0) {
			Console.WriteLine ("a forced unwind entered a managed catch");
			return 1;
		}
		if (Volatile.Read (ref ran_finally) != 0) {
			Console.WriteLine ("a forced unwind ran a managed finally");
			return 2;
		}

		Environment.Exit (42);
		return 3;
	}
}
