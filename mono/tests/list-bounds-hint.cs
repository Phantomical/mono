using System;
using System.Collections.Generic;

/* Exercise List<T> indexers with and without the LLVM bounds-check hint. */

public class ListBoundsHint {
	static int failures;

	static void Check (string what, int got, int want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
		++failures;
	}

	static int SumViaIndexer (List<int> list)
	{
		int total = 0;

		for (int i = 0; i < list.Count; ++i)
			total += list[i];

		return total;
	}

	static int Growth ()
	{
		var list = new List<int> ();

		for (int i = 0; i < 37; ++i)
			list.Add (i);

		return SumViaIndexer (list);
	}

	static int InsertWithSpareCapacity ()
	{
		var list = new List<int> (8);

		for (int i = 0; i < 4; ++i)
			list.Add (i);

		list.Insert (2, 99);

		return SumViaIndexer (list) + list[2];
	}

	static int RemoveShiftsTail ()
	{
		var list = new List<int> ();

		for (int i = 0; i < 10; ++i)
			list.Add (i);

		list.RemoveAt (0);

		return list[0] + list[list.Count - 1];
	}

	static int SetThenGet ()
	{
		var list = new List<int> { 0, 0, 0, 0, 0 };

		for (int i = 0; i < list.Count; ++i)
			list[i] = i * i;

		return SumViaIndexer (list);
	}

	static int BoundaryIndices ()
	{
		var list = new List<int> ();

		for (int i = 0; i < 5; ++i)
			list.Add (i * 10);

		return list[0] + list[list.Count - 1];
	}

	static void Round ()
	{
		Check ("growth past capacity", Growth (), 666);
		Check ("insert with spare capacity", InsertWithSpareCapacity (), 105 + 99);
		Check ("remove shifts the tail", RemoveShiftsTail (), 1 + 9);
		Check ("set then get", SetThenGet (), 0 + 1 + 4 + 9 + 16);
		Check ("boundary indices", BoundaryIndices (), 0 + 40);
	}

	public static int Main ()
	{
		// The first rounds run at tier 0, and the later ones run whatever the
		// thresholds promoted. Both answer through this same code. The count is
		// what gives a promotion time to land: a tier-1 compile is asynchronous,
		// and a few hundred tier-0 rounds are over before one arrives.
		for (int i = 0; i < 20000; ++i)
			Round ();

		if (failures != 0) {
			Console.WriteLine ("{0} wrong answers", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
