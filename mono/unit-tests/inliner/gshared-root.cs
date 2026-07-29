using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// The root itself is gshared.  GsharedCallsGshared<string> is called directly and
// repeatedly rather than through a non-generic wrapper, so when IT promotes its compile
// has cfg->gshared set.  Identity and Mix are shared over exactly the type parameter
// the root is shared over, so they share its runtime generic context and are
// materialized as the shared bodies they are.
//
// Mix<T_REF> promotes as a gshared root of its own, and the Box`1<T_REF>:.ctor it calls
// is where the refusal lands: an already-shared callee reachable only from a gshared
// root, with no exact instantiation to compile, so it keeps its trampoline call.

public class GsharedRoot {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (GsharedRoot), args);
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
			return a;
		var b = new Box<T> (a);
		return b.Value;
	}

	static T Identity<T> (T a) { return a; }

	static T GsharedCallsGshared<T> (T a) {
		return Identity (Mix (a, 0));
	}

	// INLINER-EXPECT: exposed GsharedRoot:Identity<T_REF> (T_REF)
	// INLINER-EXPECT: exposed GsharedRoot:Mix<T_REF> (T_REF,int)
	// INLINER-EXPECT: refused GsharedRoot/Box`1<T_REF>:.ctor (T_REF)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_gshared_root_shared_callee_refused () {
		string last = null;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			last = GsharedCallsGshared<string> ("g" + (i % 7));
		string expected = "g" + ((ITERS - 1) % 7);
		return last == expected ? 0 : 1;
	}
}
