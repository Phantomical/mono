/*
 * The assembly a registered override names.  Built twice under two names, so
 * that the registry is checked against two loaded copies of one target - which
 * is what a Harmony-using process looks like, each mod shipping its own repack.
 */

using System;

namespace Mono.Test {

public class Target {
	/* Small enough for the interpreter to copy into its callers. */
	public static int Value (int x)
	{
		return x + 1;
	}

	public static int Generic<T> (int x)
	{
		return x + 2;
	}

	public static int Wide (int a, int b, int c, int d, int e, int f, int g, int h)
	{
		return a + b + c + d + e + f + g + h;
	}

	public static int CallValue (int x)
	{
		return Value (x);
	}

	public static int CallGeneric (int x)
	{
		return Generic<int> (x);
	}

	public static int CallWide (int x)
	{
		return Wide (x, 0, 0, 0, 0, 0, 0, 0);
	}

	public static int Main ()
	{
		return 0;
	}
}

}
