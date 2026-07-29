using System;
using System.Runtime.CompilerServices;

// The point of the pass is not the direct call, it is what the inliner can then
// do with it -- resolving to something the inliner does not recognize would
// leave it green but pointless, so assert the fold as well.

class CalleeInlines {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (CalleeInlines), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Iters () { return 20000; }

	class Animal {
		public virtual string Noise () { return "?"; }
	}

	class Bird : Animal {
		public override string Noise () { return "tweet"; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FoldHot (int x) {
		Animal a = new Bird ();
		return a.Noise ().Length + (x & 1);
	}

	// DEVIRT-EXPECT: devirt CalleeInlines/Bird:Noise ()
	// INLINER-EXPECT: folded CalleeInlines/Bird:Noise ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_devirtualized_callee_inlines () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += FoldHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += 5 + (i & 1);          // "tweet".Length
		return sum == expected ? 0 : 1;
	}
}
