using System;
using System.Runtime.CompilerServices;

// A call in a try region is emitted as an invoke, and the inliner declines to
// fold a clause-bearing body into one -- so this is the devirtualized site that
// stays a call through the trampoline, imt argument still in tow.

class InterfaceInvoke {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (InterfaceInvoke), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Iters () { return 20000; }

	struct Pair { public int A, B; }

	interface ICount { int Count (Pair p, int k); }

	class Counter : ICount {
		public int Count (Pair p, int k) {
			if (k < 0)
				throw new ArgumentException ();
			return p.A - p.B + k;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int InvokeHot (int x) {
		ICount c = new Counter ();
		Pair p; p.A = x * 3; p.B = x;
		try {
			return c.Count (p, (x & 1) == 0 ? 1 : -1);
		} catch (ArgumentException) {
			return 100;
		}
	}

	// DEVIRT-EXPECT: devirt InterfaceInvoke/Counter:Count (InterfaceInvoke/Pair,int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_interface_invoke_devirts () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += InvokeHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += (i & 1) == 0 ? i * 3 - i + 1 : 100;
		return sum == expected ? 0 : 1;
	}
}
