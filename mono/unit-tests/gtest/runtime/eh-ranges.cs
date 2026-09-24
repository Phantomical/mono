using System;
using System.Runtime.CompilerServices;

/*
 * The methods test-eh-ranges.cpp compiles at tier 2 and reads the published
 * clauses of. Inner's catch is the only ArgumentException clause, so the test
 * finds Inner's clause in Outer's body by its class.
 */
public class EHRanges
{
	static int sink;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Foo (int a)
	{
		if (a == int.MinValue)
			throw new ArgumentException ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void More (int a)
	{
		if (a == int.MinValue)
			throw new Exception ();
	}

	static int Inner (int a, int b)
	{
		try {
			Foo (a);
		} catch (ArgumentException) {
			return -1;
		}
		return a * b + (a ^ b);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Outer (int i)
	{
		try {
			int r = Inner (i, i + 1);
			sink = r;
			More (i);
			return r;
		} catch (Exception) {
			return -2;
		}
	}

	public static int Main ()
	{
		return 0;
	}
}
