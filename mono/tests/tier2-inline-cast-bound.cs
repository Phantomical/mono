using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

/*
 * A type test whose declaring function takes a wider bound than any one
 * caller's own argument type. fold_type_tests () answers what a candidate's
 * own declared parameter settles, when the tier-2 inliner materializes it
 * standalone for costing. A caller's narrower bound answers the rest, once
 * the candidate is folded in and the caller's own compile reaches the copy
 * again.
 */

class PlainSource : IEnumerable<int> {
	public IEnumerator<int> GetEnumerator ()
	{
		yield return 1;
	}

	IEnumerator IEnumerable.GetEnumerator () => GetEnumerator ();
}

public class InlineCastBound {
	static int failures;

	static void Check (string what, int got, int want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
		++failures;
	}

	// Declared with the interface, the shape a LINQ builder's own source
	// parameter takes, so a candidate the tier-2 inliner materializes on its
	// own can only answer these against the interface itself.
	static int Specialize (IEnumerable<int> source)
	{
		if (source is ICollection<int>)
			return 1;
		if (source is List<int>)
			return 2;
		return 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ComputeList (List<int> list) => Specialize (list);

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ComputePlain (PlainSource source) => Specialize (source);

	static void Round ()
	{
		Check ("List<int> reaches ICollection<int>", ComputeList (new List<int> ()), 1);
		Check ("a class implementing neither reaches neither",
		       ComputePlain (new PlainSource ()), 0);
	}

	public static int Main ()
	{
		// The later rounds promote through both tiers, which is where
		// Specialize's cascade folds into each caller with that caller's own
		// operand rather than Specialize's own declared parameter.
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
