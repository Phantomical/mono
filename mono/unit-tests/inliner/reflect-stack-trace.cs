using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// The most sensitive of the frame-walking fixtures: if the callee's own frame were
// folded away, frame 0 would resolve to the caller instead.

public class ReflectStackTrace {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (ReflectStackTrace), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int LeafStackTraceFrame () {
		var st = new StackTrace ();
		var frame = st.GetFrame (0);
		var m = frame != null ? frame.GetMethod () : null;
		return (m != null && m.Name == "LeafStackTraceFrame") ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReflectStackTraceHotCaller () {
		return LeafStackTraceFrame ();
	}

	public static int test_0_reflect_stack_trace_frame () {
		int sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReflectStackTraceHotCaller ();
		return sum == ITERS ? 0 : 1;
	}
}
