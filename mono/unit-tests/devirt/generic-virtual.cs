using System;
using System.Runtime.CompilerServices;

// A generic virtual dispatches through the imt argument rather than a plain
// vtable slot: the generic method definition holds the slot, and the result is
// inflated with the call site's own method context afterwards.  Getting that
// wrong lands on the definition instead of the instantiation.

class GenericVirtual {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (GenericVirtual), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Iters () { return 20000; }

	class Wrapper {
		public virtual string Describe<T> (T value) { return "base:" + value; }
	}

	class LoudWrapper : Wrapper {
		public override string Describe<T> (T value) { return "loud:" + value; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenericVirtualHot (int x) {
		Wrapper w = new LoudWrapper ();
		return w.Describe<int> (x).Length;
	}

	// DEVIRT-EXPECT: devirt GenericVirtual/LoudWrapper:Describe<int> (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_virtual_devirts () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += GenericVirtualHot (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += ("loud:" + (i % 100)).Length;
		return sum == expected ? 0 : 1;
	}
}
