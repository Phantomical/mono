using System;
using System.Runtime.CompilerServices;

/* Exercise the Array.Clear memset lowering and its exception behavior. */

struct NoRefs {
	public int A;
	public long B;
}

public class ArrayClearMemset {
	static int failures;

	static void Check (string what, bool ok)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++failures;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ClearInts (int[] a, int index, int length)
	{
		Array.Clear (a, index, length);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ClearBytes (byte[] a, int index, int length)
	{
		Array.Clear (a, index, length);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ClearStructs (NoRefs[] a, int index, int length)
	{
		Array.Clear (a, index, length);
	}

	static bool Throws<T> (Action body) where T : Exception
	{
		try {
			body ();
		} catch (Exception e) {
			if (typeof (T).IsInstanceOfType (e))
				return true;

			Console.WriteLine ("FAIL: wrong exception {0}", e.GetType ());
			return false;
		}

		return false;
	}

	static void CheckInts ()
	{
		int[] middle = { 1, 2, 3, 4, 5 };

		ClearInts (middle, 1, 3);
		Check ("int[] middle", middle[0] == 1 && middle[1] == 0 && middle[2] == 0
		                       && middle[3] == 0 && middle[4] == 5);

		int[] zero = { 1, 2, 3 };

		ClearInts (zero, 1, 0);
		Check ("int[] zero length", zero[0] == 1 && zero[1] == 2 && zero[2] == 3);

		int[] zeroAtEnd = { 1, 2, 3 };

		ClearInts (zeroAtEnd, 3, 0);
		Check ("int[] zero length at the end",
		      zeroAtEnd[0] == 1 && zeroAtEnd[1] == 2 && zeroAtEnd[2] == 3);

		int[] whole = { 1, 2, 3 };

		ClearInts (whole, 0, 3);
		Check ("int[] whole array", whole[0] == 0 && whole[1] == 0 && whole[2] == 0);

		ClearInts (Array.Empty<int> (), 0, 0);
	}

	static void CheckBytes ()
	{
		byte[] a = { 1, 2, 3, 4, 5 };

		ClearBytes (a, 2, 2);
		Check ("byte[] middle", a[0] == 1 && a[1] == 2 && a[2] == 0 && a[3] == 0 && a[4] == 5);
	}

	static void CheckStructs ()
	{
		NoRefs[] a = new NoRefs[3];

		for (int i = 0; i < a.Length; ++i)
			a[i] = new NoRefs { A = i + 1, B = i + 10 };

		ClearStructs (a, 1, 1);
		Check ("struct[] middle", a[0].A == 1 && a[0].B == 10
		                         && a[1].A == 0 && a[1].B == 0
		                         && a[2].A == 3 && a[2].B == 12);
	}

	static void CheckBadRanges ()
	{
		int[] a = { 1, 2, 3 };

		Check ("negative index", Throws<IndexOutOfRangeException> (() => ClearInts (a, -1, 1)));
		Check ("negative length", Throws<IndexOutOfRangeException> (() => ClearInts (a, 0, -1)));
		Check ("index + length past the end",
		      Throws<IndexOutOfRangeException> (() => ClearInts (a, 2, 2)));
		Check ("index past the end, zero length",
		      Throws<IndexOutOfRangeException> (() => ClearInts (a, 4, 0)));
		Check ("null array", Throws<ArgumentNullException> (() => ClearInts (null, 0, 0)));
	}

	public static int Main ()
	{
		for (int i = 0; i < 20000; ++i) {
			CheckInts ();
			CheckBytes ();
			CheckStructs ();
			CheckBadRanges ();
		}

		if (failures != 0) {
			Console.WriteLine ("{0} wrong answers", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
