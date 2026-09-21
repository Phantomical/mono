using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

// An interface bitmap miss is conclusive for invariant interfaces. Variant
// and array-special interfaces still require the general cast path because
// covariance can succeed without the exact interface id in the bitmap.

interface IMarker { }
interface IPlain<T> { }
interface ICo<out T> { }

class Base { }
class Derived : Base, IMarker, IPlain<int>, ICo<Derived> { }

class Test {
	static int failures;

	static void Check (string what, bool got, bool want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
		++failures;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsMarker (object o) => o is IMarker;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsPlainInt (object o) => o is IPlain<int>;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsCoBase (object o) => o is ICo<Base>;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsIListOfBase (object o) => o is IList<Base>;

	static int Main ()
	{
		object derived = new Derived ();
		object baseObj = new Base ();
		object derivedArray = new Derived[3];

		Check ("marker/derived", IsMarker (derived), true);
		Check ("marker/base", IsMarker (baseObj), false);

		Check ("plain/derived", IsPlainInt (derived), true);
		Check ("plain/base", IsPlainInt (baseObj), false);

		// The exact ICo<Base> bit is absent, but covariance makes the cast valid.
		Check ("covariant/derived-as-base", IsCoBase (derived), true);
		Check ("covariant/base", IsCoBase (baseObj), false);

		// The exact IList<Base> bit is absent, but array covariance makes the
		// cast valid.
		Check ("array-covariant/derived-array-as-base", IsIListOfBase (derivedArray), true);
		Check ("array-covariant/base", IsIListOfBase (baseObj), false);

		return failures;
	}
}
