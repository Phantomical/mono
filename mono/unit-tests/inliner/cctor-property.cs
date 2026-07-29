using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// The same cctor barrier, reached through a property getter.

public class CctorProperty {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (CctorProperty), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static class CctorProp {
		static int _val;
		static CctorProp () { _val = 777; }
		public static int Val {
			get {
				int p1 = _val + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
				int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 / 3 + 1;
				int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7;
				if (p14 == int.MinValue)
					return -1;
				return _val;
			}
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadCctorPropHotCaller (int x) {
		return x + CctorProp.Val;
	}

	// INLINER-EXPECT: folded CctorProperty/CctorProp:get_Val ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_property_getter () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReadCctorPropHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 777;
		return sum == expected ? 0 : 1;
	}
}
