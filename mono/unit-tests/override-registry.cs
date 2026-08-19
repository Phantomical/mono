/*
 * An override assembly, in the shape the runtime reads beside itself as
 * mono-overrides.dll.  It declares the attribute rather than referencing one,
 * because the runtime matches the attribute by name in the assembly it is
 * reading.
 */

using System;

namespace Mono.Overrides {

[AttributeUsage (AttributeTargets.Method)]
public class MonoOverrideAttribute : Attribute {
	public MonoOverrideAttribute (string target)
	{
		Target = target;
	}

	public string Target { get; private set; }
}

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
