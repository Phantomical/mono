using System;
using System.Runtime.CompilerServices;

// A merge of two allocations of the same class is still exact.

class PhiSameClass {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (PhiSameClass), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Iters () { return 20000; }

	class Animal {
		public virtual int Legs () { return 0; }
	}

	class Bird : Animal {
		public override int Legs () { return 2; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PhiSameHot (int x) {
		Animal a = (x & 1) == 0 ? new Bird () : (Animal) new Bird ();
		return a.Legs ();
	}

	// DEVIRT-EXPECT: devirt PhiSameClass/Bird:Legs ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_phi_of_same_class_devirts () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += PhiSameHot (i);
		return sum == (long) ITERS * 2 ? 0 : 1;
	}
}
