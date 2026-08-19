/*
 * An override assembly that takes its attributes from corlib, which is the
 * ordinary case. override-target.cs is the other one, declaring its own.
 */

using System;

[assembly: Mono.Overrides.MonoOverrideAssembly]

namespace Mono.Overrides {

public class TargetOverrides {
	[MonoOverride ("Mono.Test.Target:Value")]
	static int Value (int x)
	{
		return x + 100;
	}

	[MonoOverride ("Mono.Test.Target:Generic")]
	static int Generic<T> (int x)
	{
		return x + 200;
	}

	[MonoOverride ("Mono.Test.Target:Wide")]
	static int Wide (int a, int b, int c, int d, int e, int f, int g, int h)
	{
		return a + b + c + d + e + f + g + h + 300;
	}
}

public class Entry {
	public static int Main ()
	{
		return 0;
	}
}

}
