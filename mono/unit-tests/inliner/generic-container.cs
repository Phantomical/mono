using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A small generic container helper over a value type.  The ctor + Swap () calls
// make it non-leaf, so it is refused regardless of genericity.  Swap ()'s padding
// is a fixed-seed int chain because T is unconstrained -- there is no arithmetic on
// First/Second available to pad with.

public class GenericContainer {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (GenericContainer), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	class Pair<T> {
		public T First, Second;
		public Pair (T a, T b) { First = a; Second = b; }

		public void Swap () {
			int p1 = 5, p2 = p1 * 3, p3 = p2 - 7, p4 = p3 ^ 11, p5 = p4 & 0xFF;
			int p6 = p5 | 0x20, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
			int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
			if (p15 == int.MinValue)
				return;			// unreachable - the seed above is fixed
			T t = First;
			First = Second;
			Second = t;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenericContainerHelper (int a, int b) {
		var p = new Pair<int> (a, b);
		p.Swap ();
		return p.First * 1000 + p.Second;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_container_helper () {
		int ITERS = Iters ();
		long sum = 0;
		for (int i = 0; i < ITERS; i++)
			sum += GenericContainerHelper (i, i + 1);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += (i + 1) * 1000 + i;
		return sum == expected ? 0 : 1;
	}
}
