using System;
using System.Runtime.CompilerServices;

// The parameter after the imt argument is one the ABI decorates, so a body
// materialized to the wrong shape lands the byval on the neighbouring argument.

class InterfaceStructArg {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (InterfaceStructArg), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Iters () { return 20000; }

	struct Pair { public int A, B; }

	interface ISum { int Add (Pair p, int k); }

	class Adder : ISum {
		public int Add (Pair p, int k) { return p.A + p.B + k; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int IfaceStructHot (int x) {
		ISum s = new Adder ();
		Pair p; p.A = x; p.B = x * 2;
		return s.Add (p, 5);
	}

	// DEVIRT-EXPECT: devirt InterfaceStructArg/Adder:Add (InterfaceStructArg/Pair,int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_interface_struct_arg_devirts () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += IfaceStructHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + i * 2 + 5;
		return sum == expected ? 0 : 1;
	}
}
