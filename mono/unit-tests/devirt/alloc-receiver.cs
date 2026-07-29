using System;
using System.Runtime.CompilerServices;

// The base case: the receiver is allocated in the method doing the call.

class AllocReceiver {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (AllocReceiver), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Iters () { return 20000; }

	class Animal {
		public virtual int Legs () { return 0; }
	}

	class Dog : Animal {
		public override int Legs () { return 4; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FreshHot (int x) {
		Animal a = new Dog ();
		return a.Legs () + (x & 1);
	}

	// DEVIRT-EXPECT: devirt AllocReceiver/Dog:Legs ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_alloc_receiver_devirts () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += FreshHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += 4 + (i & 1);
		return sum == expected ? 0 : 1;
	}
}
