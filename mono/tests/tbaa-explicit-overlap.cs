/*
 * The explicit-layout rule the `!tbaa` split rests on.
 *
 * ECMA-335 II.10.7 lets two value-typed fields share an offset and forbids a
 * reference sharing one with anything else. mono_class_layout_fields ()
 * enforces the second half (mono/metadata/class-init.c), which is what lets the
 * back end put a reference access and a scalar access in disjoint alias classes.
 *
 * Both arms matter. Without the legal arm the check could reject everything and
 * still pass.
 */

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

/// Legal: two scalars at one offset. II.10.7 permits this on purpose.
[StructLayout (LayoutKind.Explicit)]
struct LegalUnion {
	[FieldOffset (0)] public double AsDouble;
	[FieldOffset (0)] public long AsLong;
	[FieldOffset (0)] public int Low;
}

/// Illegal: a reference sharing storage with a scalar.
[StructLayout (LayoutKind.Explicit)]
class OverlappedReference {
	[FieldOffset (0)] public object Reference;
	[FieldOffset (0)] public long Bits;
}

static class Program {
	static int fails;

	static void Fail (string what)
	{
		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	/*
	 * NoInlining keeps the failure at the call. The trivial-inline pre-pass
	 * takes a body that makes an object and stores a field, and a folded copy
	 * loads the type while Main is translated, which is before the try below
	 * can catch anything.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void TouchOverlapped ()
	{
		OverlappedReference bad = new OverlappedReference ();

		bad.Bits = 1;
	}

	public static int Main ()
	{
		LegalUnion u = new LegalUnion ();

		u.AsLong = 0x4010000000000000L;
		if (u.AsDouble != 4.0)
			Fail ("a legal scalar union did not round-trip");
		if (u.Low != 0)
			Fail ("a legal scalar union read the wrong low word");

		/*
		 * Both engines raise the failure mono_class_layout_fields () recorded,
		 * so this catch stays narrow. Widening it hides an engine that raises
		 * something else.
		 */
		try {
			TouchOverlapped ();
			Fail ("a reference overlapping a scalar loaded");
		} catch (TypeLoadException e) {
			if (e.Message.IndexOf ("OverlappedReference", StringComparison.Ordinal) < 0)
				Fail ("the refusal did not name the type: " + e.Message);
		}

		Console.WriteLine (fails == 0 ? "OK" : fails + " failures");
		return fails == 0 ? 0 : 1;
	}
}
