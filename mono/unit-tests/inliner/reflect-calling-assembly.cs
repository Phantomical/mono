using System;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// GetCallingAssembly () walks one frame up, so it too answers differently if the
// callee's frame is folded away.

public class ReflectCallingAssembly {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (ReflectCallingAssembly), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int LeafCallingAssembly () {
		var asm = Assembly.GetCallingAssembly ();
		return asm != null ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReflectCallingAssemblyHotCaller () {
		return LeafCallingAssembly ();
	}

	public static int test_0_reflect_calling_assembly () {
		int sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReflectCallingAssemblyHotCaller ();
		return sum == ITERS ? 0 : 1;
	}
}
