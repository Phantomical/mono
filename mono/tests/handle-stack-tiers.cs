using System;
using System.Runtime.CompilerServices;

//
// The same leak as handle-stack-abort.cs, with two interpreted frames skipped
// by one resume rather than one:
//
//   Catcher          compiled, catches
//     InterpMe_outer interpreted
//       Middle       compiled
//         InterpMe_inner interpreted, throws
//
// Restoring only the innermost frame's handles leaves the outer one's behind,
// and that half-fix passes the single-frame case. So the shape is pinned rather
// than left to promotion: MONO_LLVM_JIT_TIER0 names the two methods that are to
// stay interpreted, and everything else is compiled.
//
// Run from a finalizer because that is where a leaked handle is visible - the
// finalizer thread asserts its handle stack is empty each time round its loop.
// That assertion is SGen's, so the Boehm arm cannot report the leak.
//
class Leaky {
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void InterpMe_inner ()
	{
		throw new InvalidOperationException ("from the inner interpreted frame");
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Middle ()
	{
		InterpMe_inner ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void InterpMe_outer ()
	{
		Middle ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Catcher ()
	{
		try {
			InterpMe_outer ();
		} catch (InvalidOperationException) {
		}
	}

	~Leaky ()
	{
		Catcher ();
	}
}

public class Test {
	public static int Main ()
	{
		for (int round = 0; round < 10; round++) {
			for (int i = 0; i < 20; i++)
				GC.KeepAlive (new Leaky ());

			GC.Collect ();
			GC.WaitForPendingFinalizers ();
		}

		Console.WriteLine ("survived");
		return 0;
	}
}
