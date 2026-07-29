using System;
using System.Runtime.CompilerServices;

// A merge of different classes must stay indirect.  If the walk ever answers
// with a common base instead of requiring the arms to agree, the site resolves
// to Animal:Weight () and the sum comes out wrong on the tier-1 run only.
//
// No tag expectation on purpose: LLVM is free to sink the call into both
// predecessors, at which point each copy has an exact receiver and both
// legitimately resolve, so asserting a refusal would be asserting that
// SimplifyCFG did not fire.  The answer is what has to hold either way.

class PhiDifferentClasses {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (PhiDifferentClasses), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Iters () { return 20000; }

	class Animal {
		public virtual int Weight () { return 0; }
	}

	class Dog : Animal {
		public override int Weight () { return 30; }
	}

	class Bird : Animal {
		public override int Weight () { return 1; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PhiDifferentHot (int x) {
		Animal a = (x & 1) == 0 ? (Animal) new Dog () : new Bird ();
		return a.Weight ();
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_phi_of_different_classes_stays_correct () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += PhiDifferentHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += (i & 1) == 0 ? 30 : 1;
		return sum == expected ? 0 : 1;
	}
}
