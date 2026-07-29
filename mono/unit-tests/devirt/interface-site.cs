using System;
using System.Runtime.CompilerServices;

// An interface site carries its imt argument in the nest parameter and the
// rewrite keeps it, which is what leaves the call a plain operand swap.

class InterfaceSite {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (InterfaceSite), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Iters () { return 20000; }

	interface IShout { int Volume (); }

	class Animal {
		public virtual int Legs () { return 0; }
	}

	class Loud : Animal, IShout {
		public int Volume () { return 11; }
		public override int Legs () { return 1; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int IfaceHot (int x) {
		IShout s = new Loud ();
		return s.Volume () + (x & 1);
	}

	// DEVIRT-EXPECT: devirt InterfaceSite/Loud:Volume ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_interface_site_devirts () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += IfaceHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += 11 + (i & 1);
		return sum == expected ? 0 : 1;
	}
}
