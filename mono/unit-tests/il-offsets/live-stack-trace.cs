using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;

// The same question for a trace taken off the running stack rather than out of an
// exception.  It reaches the runtime through a different icall
// (ves_icall_get_frame_info, one frame per call, driven by a skip count) than the
// exception scenarios, so a frame can be missing from one and present in the
// other.

class LiveStackTrace {

	static void Dump (string label, StackTrace st)
	{
		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			var m = f.GetMethod ();
			if (m == null)
				continue;
			Console.WriteLine ("{0}\t{1}:{2}\t{3}:{4}", label, m.DeclaringType.Name, m.Name,
					   Path.GetFileName (f.GetFileName ()), f.GetFileLineNumber ());
			if (m.Name == "Scenario")
				break;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void LiveLeaf ()
	{
		Dump ("live-stack", new StackTrace (true));	// IL-FRAME: live-stack 0 LiveStackTrace:LiveLeaf
	}

	// Deliberately not NoInlining, so that a front end which started folding
	// callees in would lose this frame and be caught doing it.
	static void LiveMiddle (int x)
	{
		if (x == 7)
			LiveLeaf ();	// IL-FRAME: live-stack 1 LiveStackTrace:LiveMiddle
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void LiveCaller (int x)
	{
		LiveMiddle (x);	// IL-FRAME: live-stack 2 LiveStackTrace:LiveCaller
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Scenario ()
	{
		for (int i = 0; i < 3; i++)
			LiveCaller (i);
		LiveCaller (7);	// IL-FRAME: live-stack 3 LiveStackTrace:Scenario
	}

	public static int Main ()
	{
		Scenario ();
		return 0;
	}
}
