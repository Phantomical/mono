/*
 * The assembly a registered override names.  Built twice under two names, so
 * that the registry is checked against two loaded copies of one target - which
 * is what a Harmony-using process looks like, each mod shipping its own repack.
 *
 * It also declares its own copies of the two attributes and carries overrides
 * of its own, so it covers the assembly that cannot reference this runtime's
 * corlib and the assembly that loads long after startup.  override-registry.cs
 * is the other half, taking its attributes from corlib.
 */

using System;
using System.Reflection;
using System.Runtime.CompilerServices;

[assembly: Mono.Overrides.MonoOverrideAssembly]

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

	/* Installed through the icall rather than through an attribute. */
	public static int Handled (int x)
	{
		return x + 3;
	}

	public static int HandledReplacement (int x)
	{
		return x + 400;
	}

	public static void InstallHandled ()
	{
		Mono.Overrides.MonoOverride.Install (
			typeof (Target).GetMethod ("Handled").MethodHandle.Value,
			typeof (Target).GetMethod ("HandledReplacement").MethodHandle.Value);
	}

	public static int CallHandled (int x)
	{
		return Handled (x);
	}

	/* Replaced by this assembly's own override, below. */
	public static int SelfDeclared (int x)
	{
		return x + 5;
	}

	public static int CallSelfDeclared (int x)
	{
		return SelfDeclared (x);
	}

	public static int Main ()
	{
		return 0;
	}
}

}

namespace Mono.Overrides {

/*
 * Declared here rather than taken from corlib, which is what an assembly built
 * against a different mscorlib has to do.  The runtime matches both attributes
 * by name, so these stand in for corlib's.
 */
[AttributeUsage (AttributeTargets.Assembly)]
public sealed class MonoOverrideAssemblyAttribute : Attribute {
}

[AttributeUsage (AttributeTargets.Method)]
public sealed class MonoOverrideAttribute : Attribute {
	public MonoOverrideAttribute (string target)
	{
		Target = target;
	}

	public string Target { get; private set; }
}

/* The runtime registers this as it starts. */
public static class MonoOverride {
	[MethodImpl (MethodImplOptions.InternalCall)]
	public static extern void Install (IntPtr target, IntPtr replacement);
}

public class SelfOverrides {
	[MonoOverride ("Mono.Test.Target:SelfDeclared")]
	static int SelfDeclared (int x)
	{
		return x + 500;
	}
}

}
