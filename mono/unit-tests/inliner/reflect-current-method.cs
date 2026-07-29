using System;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A frame-walking callee: GetCurrentMethod () reports the frame of the method making
// the call, so folding that method into its caller changes the answer.

public class ReflectCurrentMethod {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (ReflectCurrentMethod), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int LeafGetCurrentMethod () {
		var m = MethodBase.GetCurrentMethod ();
		return (m != null && m.Name == "LeafGetCurrentMethod") ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReflectCurrentMethodHotCaller () {
		return LeafGetCurrentMethod ();
	}

	public static int test_0_reflect_get_current_method () {
		int sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReflectCurrentMethodHotCaller ();
		return sum == ITERS ? 0 : 1;
	}
}
