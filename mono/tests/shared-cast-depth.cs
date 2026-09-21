using System;
using System.Runtime.CompilerServices;

interface IMarker { }

class Base { }
class Derived : Base { }
class Other : Base, IMarker { }

public class SharedCastDepth {
	static int failures;

	static void Check (string what, bool got, bool want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
		++failures;
	}

	// One shared body serves every reference-type T: the class T is bound to,
	// and the supertype-chain depth to test against it, both come from the
	// generic context rather than from this method's own compile.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsT<T> (object obj) => obj is T;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsTProtected<T> (object obj)
	{
		try {
			return obj is T;
		} catch (Exception) {
			return false;
		}
	}

	static void Round ()
	{
		Derived derived = new Derived ();
		Base plainBase = new Base ();
		Other other = new Other ();
		int[] ints = new int[2];

		// A reference T that matches.
		Check ("Derived is Base via T", IsT<Base> (derived), true);
		Check ("Derived is Derived via T", IsT<Derived> (derived), true);

		// A reference T that does not match.
		Check ("Base is Derived via T", IsT<Derived> (plainBase), false);
		Check ("null is Derived via T", IsT<Derived> (null), false);

		// T bound to an interface: the supertype chain does not decide this,
		// so the depth test must not be taken for it.
		Check ("Other is IMarker via T", IsT<IMarker> (other), true);
		Check ("Derived is IMarker via T", IsT<IMarker> (derived), false);

		// T bound to an array: covariance makes the depth chain wrong here too.
		Check ("int[] is int[] via T", IsT<int[]> (ints), true);
		Check ("Base is int[] via T", IsT<int[]> (plainBase), false);

		// A value-type T, excluded from the depth test as well.
		Check ("boxed int is int via T", IsT<int> (5), true);
		Check ("boxed long is int via T", IsT<int> (5L), false);
		Check ("Base is int via T", IsT<int> (plainBase), false);

		// A clause around the site, which turns it into an invoke: the depth
		// test's own fallback and its miss both have to reach the same handler
		// the site's own unwind edge named.
		Check ("protected Derived is Base via T", IsTProtected<Base> (derived), true);
		Check ("protected Base is Derived via T", IsTProtected<Derived> (plainBase), false);
	}

	public static int Main ()
	{
		// Long enough to reach tier 2's promotion threshold in every
		// configuration that promotes.
		for (int i = 0; i < 25000; ++i)
			Round ();

		if (failures != 0) {
			Console.WriteLine ("{0} wrong answers", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
