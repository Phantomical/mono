using System;
using System.Threading;

//
// A finalizer whose exception is caught above it, in a frame the other engine
// compiled.
//
// A finalizer body is ordinary IL, so at tier 0 it is interpreted, and the
// catch that stops an exception leaving it belongs to the runtime-invoke
// wrapper, which is a wrapper and never interpreted. Resuming into that catch
// restores the stack pointer over the interpreter's own frame, which therefore
// never reaches its exit and never hands back the handle it holds. The handle
// stays a live root, and mono_gc_run_finalize () pushes no mark of its own to
// restore, so it is still there when the finalizer thread comes back round its
// loop and marks itself unscannable - which asserts that the handle stack is
// empty.
//
// It has to be a thread abort rather than any other exception. Anything else
// leaving a finalizer is fatal, and this needs the process to survive as far as
// the next turn of the loop.
//
// The assertion belongs to SGen, so the Boehm arm of this runs the same code
// and cannot report the leak. It is the SGen arm that is the test.
//
class Leaky {
	~Leaky ()
	{
		Thread.CurrentThread.Abort ();
	}
}

public class Test {
	public static int Main ()
	{
		for (int round = 0; round < 10; round++) {
			for (int i = 0; i < 20; i++)
				GC.KeepAlive (new Leaky ());

			GC.Collect ();
			GC.WaitForPendingFinalizers ();
		}

		Console.WriteLine ("survived");
		return 0;
	}
}
