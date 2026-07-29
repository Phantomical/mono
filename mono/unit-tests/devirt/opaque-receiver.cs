using System;
using System.Runtime.CompilerServices;

// OpaqueAge is NoInlining, so it compiles as its own root with the receiver
// arriving as a plain parameter and no optimization can turn that back into an
// allocation the walk could see.

class OpaqueReceiver {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (OpaqueReceiver), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Iters () { return 20000; }

	class Animal {
		public virtual int Age () { return 0; }
	}

	class Dog : Animal {
		public override int Age () { return 7; }
	}

	class Bird : Animal {
		public override int Age () { return 3; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int OpaqueAge (Animal a) { return a.Age (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int OpaqueHot (int x) {
		return OpaqueAge ((x & 1) == 0 ? (Animal) new Dog () : new Bird ());
	}

	// DEVIRT-EXPECT: refused OpaqueReceiver/Animal:Age ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_opaque_receiver_refused () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += OpaqueHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += (i & 1) == 0 ? 7 : 3;
		return sum == expected ? 0 : 1;
	}
}
