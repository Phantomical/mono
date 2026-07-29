using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A static method on a generic class is the shape that carries a `nest` argument:
// check_method_sharing () decided from metadata that a sharable static generic
// callee is passed a vtable, and it decided that when the caller's front end ran.
// The specialized body keeps the parameter and ignores it -- dropping it would
// give the body and the declaration different LLVM types and the site could not
// be rewired at all.  The int instantiation is not sharable and is passed
// nothing, so the two together cover both paths.

public class StaticGenericMethod {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (StaticGenericMethod), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	class StaticGenericHolder<T> {
		public static int Weigh (T a, T b, bool first, int pad) {
			int p1 = pad + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
			int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
			int p11 = p10 + p3, p12 = p11 - p4;
			if (p12 == int.MinValue)
				return 0;		// unreachable for the pad range used below
			return (first ? a : b).ToString ().Length;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int StaticGenericHotString (int x) {
		string a = "a" + x, b = "bb" + x;
		return StaticGenericHolder<string>.Weigh (a, b, (x & 1) == 0, x);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int StaticGenericHotInt (int x) {
		return StaticGenericHolder<int>.Weigh (x, x + 1000, (x & 1) == 0, x);
	}

	// INLINER-EXPECT: folded StaticGenericMethod/StaticGenericHolder`1<string>:Weigh (string,string,bool,int)
	// INLINER-EXPECT: folded StaticGenericMethod/StaticGenericHolder`1<int>:Weigh (int,int,bool,int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_static_generic_class_method_inlines () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += StaticGenericHotString (i % 100) + StaticGenericHotInt (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = i % 100;
			string a = "a" + x, b = "bb" + x;
			expected += ((x & 1) == 0 ? a : b).Length;
			expected += ((x & 1) == 0 ? x : x + 1000).ToString ().Length;
		}
		return sum == expected ? 0 : 1;
	}
}
