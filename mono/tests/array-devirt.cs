using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

/*
 * A dispatch on an array receiver, and the class the compiler can answer it
 * from.
 *
 * An array slot admits every array of that rank with the same cast class. So an
 * `int[]` parameter also holds a `uint[]`, and an array of an enum over int.
 * ECMA-335 I.8.7 makes the three interchangeable as values, and `cast-fold.cs`
 * gates the type tests that follow from that.
 *
 * A dispatch does not follow from it. Each of the three classes carries a vtable
 * of its own, and the interface slot holds a different method in each. So
 * `exact_class ()` (`mono/llvm/operand-class.cpp`) refuses an array, and this
 * file fails if that refusal comes out.
 *
 * The enumerator is what separates the three, because its class names the
 * element type the slot reached. `Count` and `Contains` answer with bits that
 * all three arrays share, so neither one can fail here.
 *
 * The enum array agrees with `int[]`, and that is not a mistake in the row
 * below: a class initializer sets an enum's element class to the underlying
 * type before the array reads it. The `uint[]` row is the one that fails.
 */

enum E32 : int { A = 1, B = 2 }

public class ArrayDevirt {
	static int failures;

	static void Check (string what, string got, string want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
		++failures;
	}

	// The receiver is bounded by `int[]` and no more. A rewrite that answers off
	// that bound reaches int[]'s slot for all three arrays.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string EnumeratorType (int[] a)
	{
		return ((IEnumerable<int>) a).GetEnumerator ().GetType ().ToString ();
	}

	const string enumerator = "System.Array+InternalEnumerator`1[System.";

	static void Round (int[] ints, int[] uints, int[] enums)
	{
		Check ("int[] enumerator", EnumeratorType (ints), enumerator + "Int32]");
		Check ("uint[] enumerator", EnumeratorType (uints), enumerator + "UInt32]");
		Check ("enum[] enumerator", EnumeratorType (enums), enumerator + "Int32]");
	}

	public static int Main ()
	{
		// C# writes neither cast, because it reads the two array types as
		// unrelated. The runtime admits both, which is the point of the test.
		int[] ints = new int[] { 1, 2, 3 };
		int[] uints = (int[]) (object) new uint[] { 1, 2, 3 };
		int[] enums = (int[]) (object) new E32[] { E32.A, E32.B };

		// The first rounds run interpreted, and the later ones run whatever the
		// thresholds promoted. Both answer through this same code.
		for (int i = 0; i < 200; ++i)
			Round (ints, uints, enums);

		if (failures != 0) {
			Console.WriteLine ("{0} wrong answers", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
