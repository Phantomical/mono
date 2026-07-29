using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// A nested generic container, deeply non-leaf.  For a reference-type T every one
// of those Dictionary/List callees is a shared instantiation the inliner now
// specializes; either way it has to stay correct.

public class NestedGenerics {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (NestedGenerics), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	static int NestedGenericsContainer<T> (T a, T b) {
		var dict = new Dictionary<T, List<T>> ();
		var list = new List<T> ();
		list.Add (a);
		list.Add (b);
		dict [a] = list;
		return dict [a].Count;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int NestedGenericsHotInt (int x) {
		return NestedGenericsContainer<int> (x, x + 1);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int NestedGenericsHotString (int x) {
		return NestedGenericsContainer<string> ("a" + x, "b" + x);
	}

	public static int test_0_nested_generics_container () {
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++) {
			if (NestedGenericsHotInt (i) != 2)
				return 1;
			if (NestedGenericsHotString (i) != 2)
				return 2;
		}
		return 0;
	}
}
