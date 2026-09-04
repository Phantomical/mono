using System;
using System.Reflection;
using System.Threading;

// --llvm-opt=-mono-tier0-classic=BackedgeCounter filters BackedgeCounterLoop
// to the classic compiler. It is called exactly once, which charges its
// entry counter one unit against the default threshold of ten - well short
// of it alone. Only its loop's own back edges can spend the rest. Reaching
// tier 1 here is what tells classic tier0's back edge count apart from its
// call count.
//
// Reflection crosses into a compiled runtime-invoke wrapper, the same way
// tier0-classic-gsharedvt.cs's GsharedShareEnter does. An ordinary call from
// interpreted Main () would stay interpreted. The interpreter reaches a
// callee it can interpret itself without asking the backend, so the filter
// never gets a chance to answer for it.
namespace Mono.Tiering {
	static class MonoTier {
		[System.Runtime.CompilerServices.MethodImpl (System.Runtime.CompilerServices.MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
		[System.Runtime.CompilerServices.MethodImpl (System.Runtime.CompilerServices.MethodImplOptions.InternalCall)]
		public static extern int GetTier (IntPtr method);
	}
}

public class Tier0ClassicBackedgeTest
{
	const int tier1 = 3;

	static long BackedgeCounterLoop ()
	{
		long acc = 0;

		for (long i = 0; i < 1000; i++)
			acc += i;

		return acc;
	}

	public static int Main ()
	{
		MethodInfo method = typeof (Tier0ClassicBackedgeTest).GetMethod (
			"BackedgeCounterLoop", BindingFlags.Static | BindingFlags.NonPublic);
		IntPtr handle = method.MethodHandle.Value;

		const long want = 999L * 1000L / 2L;
		long got = (long) method.Invoke (null, null);

		if (got != want) {
			Console.WriteLine ("FAIL: wrong sum, got {0} want {1}", got, want);
			return 1;
		}

		// Promotion is queued off the call that crossed the threshold, so the
		// tier-1 body lands on a compile worker rather than on this thread.
		int tier = -1;

		for (int i = 0; i < 200 && tier < tier1; i++) {
			tier = Mono.Tiering.MonoTier.GetTier (handle);
			if (tier < tier1)
				Thread.Sleep (10);
		}

		if (tier < tier1) {
			Console.WriteLine ("FAIL: BackedgeCounterLoop () never reached tier 1, called once");
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
