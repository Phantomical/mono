using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// The reference-type twin.  GenericPick<string> is what mono compiles shared;
// materialize_callee () compiles the exact instantiation instead, which needs no
// rgctx, so it folds on the same terms as a value-type one.

public class GenericLeafReftype {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (GenericLeafReftype), args);
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
	static int GenericPickHotString (int x) {
		string a = "a" + x, b = "bb" + x;
		string r = GenericPick<string> (a, b, (x & 1) == 0, x);
		return r.Length;
	}

	// INLINER-EXPECT: folded GenericLeafReftype:GenericPick<string> (string,string,bool,int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_leaf_reftype_inlines () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += GenericPickHotString (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = i % 100;
			string a = "a" + x, b = "bb" + x;
			expected += ((x & 1) == 0 ? a : b).Length;
		}
		return sum == expected ? 0 : 1;
	}
}
