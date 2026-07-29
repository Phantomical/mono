using System;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A gshared root calling a callee shared over exactly the type parameters the root
// is shared over: same class, same instantiation, so the two share one runtime
// generic context and the call site already passes it.  That callee is
// materialized as the shared body it is rather than refused.
//
// The two instantiations run the same shared code, which is what makes this
// differential: `new T[2]` reads the element vtable out of the rgctx, so a folded
// body that lost the context -- or kept one instantiation's -- would build an array
// of the wrong element type and the store into it would throw.

public class GsharedSharedContext {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (GsharedSharedContext), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	class SharedRing<T> where T : class {
		public T Value;
		public SharedRing (T v) { Value = v; }

		public T[] Take (int pad) {
			int p1 = pad + 1, p2 = p1 * 3, p3 = p2 - 7, p4 = p3 ^ 11, p5 = p4 & 0xFF;
			int p6 = p5 | 0x20, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
			int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
			if (p15 == int.MinValue)
				return null;		// unreachable for the pad range used below
			T[] a = new T [2];
			a [0] = Value;
			a [1] = Value;
			return a;
		}

		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Hot (int x) {
			T[] a = Take (x);
			return a.Length + (a.GetType () == typeof (T[]) ? 1 : 0);
		}
	}

	// INLINER-EXPECT: folded GsharedSharedContext/SharedRing`1<T_REF>:Take (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_gshared_callee_shares_root_context () {
		var s = new SharedRing<string> ("v");
		var o = new SharedRing<object> (new object ());
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += s.Hot (i) + o.Hot (i);
		return sum == 6L * ITERS ? 0 : 1;
	}
}
