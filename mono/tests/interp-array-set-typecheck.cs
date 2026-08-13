using System;
using System.Runtime.CompilerServices;

// Storing into a multidimensional array goes through the array's Set method,
// and the check that guards it is a check on the value being stored: it has to
// be an instance of the array's real element type.
//
// The interpreter checks the call site's static element type against the real
// one instead. Where a Derived[,] is held in a Base[,] variable those differ,
// and a store of a Derived is refused although it is exactly what the array
// holds. The value decides, so the store below is legal.
class Test {
	class Base { }
	class Derived : Base { }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string StoreDerived ()
	{
		Base[,] a = new Derived[2, 2];
		// Held as a Base, so the store's static type is the one that differs
		// from the array's real element type.
		Base value = new Derived ();

		try {
			a[0, 0] = value;
			return a[0, 0] is Derived ? null : "the array kept something else";
		} catch (ArrayTypeMismatchException) {
			return "a Derived was refused by a Derived[,]";
		}
	}

	// The other half of the same check: a Base is not a Derived, so this one has
	// to be refused.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string StoreBase ()
	{
		Base[,] a = new Derived[2, 2];
		Base value = new Base ();

		try {
			a[0, 0] = value;
			return "a Base went into a Derived[,]";
		} catch (ArrayTypeMismatchException) {
			return null;
		}
	}

	public static int Main ()
	{
		int failures = 0;

		foreach (string bad in new string[] { StoreDerived (), StoreBase () }) {
			if (bad == null)
				continue;

			Console.WriteLine ("FAILED: " + bad);
			failures++;
		}

		return failures;
	}
}
