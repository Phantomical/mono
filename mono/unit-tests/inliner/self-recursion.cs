using System;
using System.Runtime.CompilerServices;

// Self-recursion, which candidate_target () excludes for free: a self-call targets the
// root's own defined Function rather than a trampoline declaration, so it never becomes
// a worklist candidate at all.  This just confirms deep recursion stays correct once
// the recursive method itself is promoted.

public class SelfRecursion {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (SelfRecursion), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long InlinerFib (int n) {
		return n < 2 ? n : InlinerFib (n - 1) + InlinerFib (n - 2);
	}

	public static int test_0_self_recursion_fib () {
		return InlinerFib (27) == 196418 ? 0 : 1;
	}
}
