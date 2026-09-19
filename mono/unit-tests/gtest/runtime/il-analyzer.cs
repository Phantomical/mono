using System;
using System.Runtime.CompilerServices;

/*
 * Method bodies inspected by test-il-analyzer.cpp. The tests compare reachable
 * IL sizes across generic instantiations; these methods are not executed.
 */
public class Analyzed
{
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Pad (int a)
	{
		return a;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Touch (ref int k)
	{
		k = 2;
	}

	public static int Straight (int x)
	{
		return x + x;
	}

	public static int Guard<T> ()
	{
		if (typeof (T) == typeof (int))
			return 1;

		int a = 0;

		a += 1; a += 2; a += 3; a += 4; a += 5; a += 6; a += 7; a += 8;
		return Pad (a);
	}

	public static int IsReference<T> ()
	{
		if (!typeof (T).IsValueType)
			return 1;

		int a = 0;

		a += 1; a += 2; a += 3; a += 4; a += 5; a += 6; a += 7; a += 8;
		return Pad (a);
	}

	public static int Boxed<T> ()
	{
		if (default (T) != null)
			return 1;

		int a = 0;

		a += 1; a += 2; a += 3; a += 4; a += 5; a += 6; a += 7; a += 8;
		return Pad (a);
	}

	public static int Switched<T> ()
	{
		int k = typeof (T) == typeof (int) ? 1 : typeof (T) == typeof (long) ? 2 : typeof (T) == typeof (short) ? 3 : 0;

		switch (k) {
		case 1:
			return 1;
		case 2:
			return 2;
		case 3:
			return 3;
		}

		int a = 0;

		a += 1; a += 2; a += 3; a += 4; a += 5; a += 6; a += 7; a += 8;
		return Pad (a);
	}

	public static int Protected<T> ()
	{
		try {
			if (typeof (T) == typeof (int))
				return 1;

			int a = 0;

			a += 1; a += 2; a += 3; a += 4; a += 5; a += 6; a += 7; a += 8;
			return Pad (a);
		} finally {
			Pad (0);
		}
	}

	public static int Looped (int n)
	{
		int k = 1;

		for (int i = 0; i < n; i++)
			k = n;

		if (k == 1)
			return 1;

		int a = 0;

		a += 1; a += 2; a += 3; a += 4; a += 5; a += 6; a += 7; a += 8;
		return Pad (a);
	}

	public static int AddressTaken ()
	{
		int k = 1;

		Touch (ref k);

		if (k == 1)
			return 1;

		int a = 0;

		a += 1; a += 2; a += 3; a += 4; a += 5; a += 6; a += 7; a += 8;
		return Pad (a);
	}

	public static int Main ()
	{
		return 0;
	}
}
