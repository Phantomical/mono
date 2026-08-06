using System;
using System.Runtime.CompilerServices;

//
// An exception that is still unwinding while a finally handler runs has to
// survive whatever that handler does. Two ways it can be lost:
//
//  - a collection moves it, so any address taken before the handler ran is
//    stale afterwards;
//  - the handler throws and catches an exception of its own, which drops the
//    reference the runtime was holding on the outer one.
//
// Either way the unwinder resumes on a dead object, and the type test against
// the catch clause reads a garbage vtable.
//

class Driver {
	class MyException : Exception {
		public int Tag;
		public MyException (int tag) { Tag = tag; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Thrower (int tag)
	{
		throw new MyException (tag);
	}

	// Moves the in-flight exception out of the nursery.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Moving (int tag)
	{
		try {
			Thrower (tag);
		} finally {
			GC.Collect (0);
		}
	}

	// Runs a complete throw/catch of its own inside the handler, then collects.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Nesting (int tag)
	{
		try {
			Thrower (tag);
		} finally {
			try {
				Thrower (-1);
			} catch (MyException) {
			}
			GC.Collect (0);
		}
	}

	static int Check (Action<int> body, int tag)
	{
		try {
			body (tag);
		} catch (MyException e) {
			return e.Tag == tag ? 0 : 2;
		}
		return 1;
	}

	static int Main ()
	{
		for (int i = 0; i < 100; i++) {
			int res = Check (Moving, i);
			if (res != 0) {
				Console.WriteLine ("moving: iteration {0} returned {1}", i, res);
				return res;
			}

			res = Check (Nesting, i);
			if (res != 0) {
				Console.WriteLine ("nesting: iteration {0} returned {1}", i, res);
				return res;
			}
		}

		Console.WriteLine ("ok");
		return 0;
	}
}
