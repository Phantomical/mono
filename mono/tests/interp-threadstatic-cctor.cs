using System;
using System.Runtime.CompilerServices;

// ECMA-335 II.10.5.3 makes the first access to any static field of a type run
// that type's initializer. A thread-static field is a static field, so reading
// one has to run it as well.
//
// The interpreter reaches a thread-static through its special-static offset
// alone. Nothing on that path carries the vtable, so nothing runs the
// initializer, and the class stays uninitialized until something touches an
// ordinary static.
class Test {
	static class Witness {
		public static int Ran;
	}

	// Nothing here but the thread-static, so the only way to reach the class is
	// through it.
	class OnlyThreadStatic {
		static OnlyThreadStatic ()
		{
			Witness.Ran = 7;
		}

		[ThreadStatic]
		public static int Slot;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadThreadStatic ()
	{
		return OnlyThreadStatic.Slot;
	}

	public static int Main ()
	{
		int slot = ReadThreadStatic ();

		if (slot != 0) {
			Console.WriteLine ("FAILED: the slot started at " + slot);
			return 1;
		}

		if (Witness.Ran != 7) {
			Console.WriteLine ("FAILED: reading the thread-static left the class "
					   + "initializer unrun");
			return 1;
		}

		return 0;
	}
}
