using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A generic method that constructs another generic type.  Both roots here are
// non-generic, so both instantiations of Mix -- and the Box ctor each one constructs --
// materialize exact and fold, including the reference-type one mono itself compiles
// shared.  gshared-root.cs is where the ctor is instead refused, reached from a root
// that has only the shared instantiation to offer.

public class GenericConstructsGeneric {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (GenericConstructsGeneric), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	class Box<T> {
		public T Value;
		public Box (T v) { Value = v; }
	}

	static T Mix<T> (T a, int pad) {
		int p1 = pad + 1, p2 = p1 * 3, p3 = p2 - 7, p4 = p3 ^ 11, p5 = p4 & 0xFF;
		int p6 = p5 | 0x20, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return a;			// unreachable for the pad range used below
		var b = new Box<T> (a);
		return b.Value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int MixHotInt (int x) {
		return Mix<int> (x, x);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int MixHotString (int x) {
		return Mix<string> ("v" + x, x).Length;
	}

	// INLINER-EXPECT: folded GenericConstructsGeneric:Mix<string> (string,int)
	// INLINER-EXPECT: folded GenericConstructsGeneric/Box`1<string>:.ctor (string)
	// INLINER-EXPECT: folded GenericConstructsGeneric/Box`1<int>:.ctor (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_constructs_generic_type () {
		long sumInt = 0, sumStr = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++) {
			sumInt += MixHotInt (i);
			sumStr += MixHotString (i % 1000);
		}
		long expectedInt = 0, expectedStr = 0;
		for (int i = 0; i < ITERS; i++) {
			expectedInt += i;
			expectedStr += ("v" + (i % 1000)).Length;
		}
		if (sumInt != expectedInt)
			return 1;
		if (sumStr != expectedStr)
			return 2;
		return 0;
	}
}
