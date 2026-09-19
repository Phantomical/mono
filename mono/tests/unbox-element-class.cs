using System;
using System.Runtime.CompilerServices;

/*
 * Exercise repeated unbox checks after tier 2 marks the element_class load
 * invariant. A mismatched object must still throw InvalidCastException.
 */

enum SampleEnum { A, B }

public class UnboxElementClass {
	static int failures;

	static void Check (string what, bool got, bool want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
		++failures;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static double Sum (object a, object b)
	{
		return (double) a + (double) b;
	}

	// The optimizer can infer the class of both boxes and remove the unbox checks.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static double SumKnownClass ()
	{
		object a = 1.5;
		object b = 2.5;

		return (double) a + (double) b;
	}

	// An enum's element_class is its underlying type, so this must resolve int
	// rather than reuse the enum's class symbol.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UnboxKnownEnumAsInt ()
	{
		object o = SampleEnum.B;

		return (int) o;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool ThrowsOnWrongClass (object o)
	{
		try {
			double unused = (double) o;
			return false;
		} catch (InvalidCastException) {
			return true;
		}
	}

	public static int Main ()
	{
		object a = 1.5;
		object b = 2.5;
		object wrong = 42;

		for (int i = 0; i < 200000; ++i) {
			Check ("same-class sum", Sum (a, b) == 4.0, true);
			Check ("wrong-class throws", ThrowsOnWrongClass (wrong), true);
			Check ("known-class sum", SumKnownClass () == 4.0, true);
			Check ("known enum unboxes as underlying type", UnboxKnownEnumAsInt () == 1, true);
		}

		if (failures != 0) {
			Console.WriteLine ("{0} wrong answers", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
