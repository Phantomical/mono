using System;
using System.Runtime.CompilerServices;

//
// An LLVM finally handler entered by unwinding does not return to its caller:
// it calls back into the runtime to resume the unwind, and the runtime parks
// everything it needs to carry on until then. Exceptions thrown from inside
// such a handler unwind through finally handlers of their own, so the parked
// states nest, and the outer one has to survive the inner ones.
//
// Two ways an inner unwind can steal the outer state:
//
//  - it parks a state of its own and resumes out of it, leaving its own frame
//    behind as the one the outer resume continues from;
//  - it abandons a handler in the same frame - a finally that throws, caught
//    by an enclosing clause - so the state parked for it is never resumed.
//
// Either way the outer handler resumes on a frame that is gone, or on the
// wrong exception.
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

	// Throws and catches through a finally of its own, one frame down.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void InnerUnwind ()
	{
		try {
			Thrower (-1);
		} finally {
		}
	}

	// The handler runs a whole unwind of its own before resuming ours.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Nested (int tag)
	{
		try {
			Thrower (tag);
		} finally {
			try {
				InnerUnwind ();
			} catch (MyException) {
			}
		}
	}

	// The handler abandons a second handler in this same frame: the inner
	// finally throws, and the throw is caught outside it.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Abandoning (int tag)
	{
		try {
			Thrower (tag);
		} finally {
			try {
				try {
					Thrower (-2);
				} finally {
					Thrower (-3);
				}
			} catch (MyException) {
			}
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
			int res = Check (Nested, i);
			if (res != 0) {
				Console.WriteLine ("nested: iteration {0} returned {1}", i, res);
				return res;
			}

			res = Check (Abandoning, i);
			if (res != 0) {
				Console.WriteLine ("abandoning: iteration {0} returned {1}", i, res);
				return res;
			}
		}

		Console.WriteLine ("ok");
		return 0;
	}
}
