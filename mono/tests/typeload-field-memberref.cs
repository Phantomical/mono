/*
 * A field token that names a class which failed to load.
 *
 * The metadata names a field of a generic instance with a MemberRef that has a
 * TypeSpec parent, even inside one assembly. So the token below resolves
 * through field_from_memberref () (mono/metadata/loader.c) rather than the
 * typedef arm beside it that tbaa-explicit-overlap.cs covers.
 *
 * Both arms matter. Without the arm that loads, nothing shows that a healthy
 * generic class still answers a field lookup through this same branch.
 */

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

/*
 * A generic type declaration with an explicit layout, which
 * mono_class_setup_fields () (mono/metadata/class-init.c) refuses. The refusal
 * records a TypeLoadException on the class, and a field lookup on a class that
 * carries one misses whatever fields the class has.
 */
[StructLayout (LayoutKind.Explicit)]
class Overlapped<T> {
	[FieldOffset (0)] public long Bits;
}

/// A generic class that loads, so its field arrives through the same branch.
class Holder<T> {
	public long Bits;
}

static class Program {
	static int fails;

	static void Fail (string what)
	{
		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	/*
	 * The compile of this method finds the class failure and makes the body
	 * throw at entry, so the call below raises inside the try. NoInlining is
	 * what keeps the site a call: may_fold () (mono/llvm/runtime/inline-scope.cpp)
	 * refuses a callee that carries it.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void TouchOverlapped (Overlapped<int> bad)
	{
		bad.Bits = 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long TouchHolder (Holder<int> good)
	{
		good.Bits = 4;

		return good.Bits;
	}

	public static int Main ()
	{
		if (TouchHolder (new Holder<int> ()) != 4)
			Fail ("a generic instance that loads did not answer its field");

		/*
		 * Both engines raise the failure the class recorded, so this catch stays
		 * narrow. A wider catch lets back the MissingFieldException this arm
		 * reported before the loader asked the class.
		 */
		try {
			TouchOverlapped (null);
			Fail ("an explicit layout on a generic type loaded");
		} catch (TypeLoadException e) {
			if (e.Message.IndexOf ("Overlapped", StringComparison.Ordinal) < 0)
				Fail ("the refusal did not name the type: " + e.Message);
		}

		Console.WriteLine (fails == 0 ? "OK" : fails + " failures");
		return fails == 0 ? 0 : 1;
	}
}
