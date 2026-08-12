using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Reflection;
using System.Threading;

// Thread.GetStackTraces walks every thread, including a thread that is running
// interpreted bytecode. The interpreter keeps the ip of a running frame in a local
// variable, so such a frame has no ip, and an offset derived from it is a number
// that looks like an offset and is not one.
class InterpFrameNativeOffset
{
	// No method in this test compiles or transforms to anything this long.
	const int PlausibleLimit = 0x100000;

	// What a walk reports for a frame whose position it does not know.
	const int Unknown = -1;

	static volatile bool stop;
	static volatile bool spinning;
	static long counter;

	// Runs for the whole test in one call, so the frame stays interpreted: nothing
	// moves a frame that is already on the stack to compiled code.
	static void Spin ()
	{
		long n = 0;

		spinning = true;
		while (!stop)
			n++;
		counter = n;
	}

	static Dictionary<Thread, StackTrace> GetStackTraces ()
	{
		var get = typeof (Thread).GetMethod ("Mono_GetStackTraces",
		                                     BindingFlags.NonPublic | BindingFlags.Static);
		if (get == null)
			throw new Exception ("no Thread.Mono_GetStackTraces");

		return (Dictionary<Thread, StackTrace>) get.Invoke (null, null);
	}

	public static int Main ()
	{
		var worker = new Thread (Spin);
		worker.IsBackground = true;
		worker.Name = "spinner";
		worker.Start ();

		while (!spinning)
			Thread.Sleep (1);
		Thread.Sleep (100);

		Dictionary<Thread, StackTrace> traces;
		try {
			traces = GetStackTraces ();
		} finally {
			stop = true;
		}
		worker.Join ();

		StackTrace trace;
		if (!traces.TryGetValue (worker, out trace)) {
			Console.WriteLine ("the walk did not report the spinning thread");
			return 1;
		}

		// Without this the test passes whatever the offsets say, because a thread
		// reported with no frames has nothing to be wrong about.
		if (trace.FrameCount == 0) {
			Console.WriteLine ("the spinning thread was reported with no frames");
			return 1;
		}

		for (int i = 0; i < trace.FrameCount; i++) {
			int offset = trace.GetFrame (i).GetNativeOffset ();
			if (offset == Unknown || (offset >= 0 && offset < PlausibleLimit))
				continue;

			Console.WriteLine ("frame {0} of {1}: offset 0x{2:x} is neither an offset nor unknown",
			                   i, trace.FrameCount, offset);
			return 1;
		}

		Console.WriteLine ("{0} frames, all offsets accounted for ({1} loops)",
		                   trace.FrameCount, counter);
		return 0;
	}
}
