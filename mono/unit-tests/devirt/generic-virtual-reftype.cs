using System;
using System.Runtime.CompilerServices;

// A generic method instantiated over a reference type is compiled shared, so a
// direct call to it would want an mrgctx in the very parameter the rewrite
// drops.  There is nothing to put there, so the site must be left alone.

class GenericVirtualReftype {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (GenericVirtualReftype), args);
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
	static int GenericVirtualRefHot (int x) {
		Wrapper w = new LoudWrapper ();
		return w.Describe<string> ("s" + x).Length;
	}

	// DEVIRT-EXPECT: refused GenericVirtualReftype/LoudWrapper:Describe<string> (string)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_virtual_reftype_refused () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += GenericVirtualRefHot (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += ("loud:s" + (i % 100)).Length;
		return sum == expected ? 0 : 1;
	}
}
