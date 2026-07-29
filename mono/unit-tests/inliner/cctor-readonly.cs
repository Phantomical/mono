using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// The same cctor barrier, on a static readonly field.

public class CctorReadonly {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (CctorReadonly), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static class ReadonlyHolder {
		public static readonly int Value;
		static ReadonlyHolder () { Value = 555; }
	}

	static int ReadReadonly (int x) {
		int p1 = x + 2, p2 = p1 * 3, p3 = p2 - 4, p4 = p3 ^ 6, p5 = p4 & 0xFF;
		int p6 = p5 | 0x08, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 / 3 + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x + ReadonlyHolder.Value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadReadonlyHotCaller (int x) {
		return ReadReadonly (x);
	}

	// INLINER-EXPECT: folded CctorReadonly:ReadReadonly (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_readonly_field () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReadReadonlyHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 555;
		return sum == expected ? 0 : 1;
	}
}
