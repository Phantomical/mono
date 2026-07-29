using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A generic leaf over a value type, which mono never shares.  The padding is an
// int-only local chain because T is unconstrained.

public class GenericLeafValuetype {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (GenericLeafValuetype), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static T GenericPick<T> (T a, T b, bool pickFirst, int pad) {
		int p1 = pad + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
		int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p3, p12 = p11 - p4;
		if (p12 == int.MinValue)
			return b;			// unreachable for the pad range used below
		return pickFirst ? a : b;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenericPickHotInt (int x) {
		return GenericPick<int> (x, x + 1000, (x & 1) == 0, x);
	}

	// INLINER-EXPECT: folded GenericLeafValuetype:GenericPick<int> (int,int,bool,int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_leaf_valuetype_inlines () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += GenericPickHotInt (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = i % 100;
			expected += (x & 1) == 0 ? x : x + 1000;
		}
		return sum == expected ? 0 : 1;
	}
}
