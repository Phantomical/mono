using System;
using System.Reflection;
using System.Reflection.Emit;

/*
 * A callvirt naming a method with no this.
 *
 * The site has no receiver to dispatch on, so it can only be an ordinary call.
 * Code generators emit this shape, and Harmony and MonoMod write patch bodies
 * with it, so a runtime that refuses it rejects assemblies mini runs.
 *
 * DynamicMethod is what makes the shape reachable. No C# compiler emits it.
 */
class Test {
	static int Helper (int x)
	{
		return x + 1;
	}

	static int Sum (int a, int b, int c)
	{
		return a + b + c;
	}

	// Emits `ldc.i4 <args> ; callvirt target ; ret`.
	static Func<int> CallvirtCaller (string name, params int[] args)
	{
		DynamicMethod dm = new DynamicMethod ("cv_" + name, typeof (int),
		                                      new Type[0], typeof (Test), true);
		ILGenerator il = dm.GetILGenerator ();

		foreach (int arg in args)
			il.Emit (OpCodes.Ldc_I4, arg);

		il.Emit (OpCodes.Callvirt, typeof (Test).GetMethod (name,
			BindingFlags.NonPublic | BindingFlags.Static));
		il.Emit (OpCodes.Ret);

		return (Func<int>) dm.CreateDelegate (typeof (Func<int>));
	}

	static int Check (string what, int got, int want)
	{
		Console.WriteLine ("{0}: got {1}, want {2}", what, got, want);
		return got == want ? 0 : 1;
	}

	public static int Main ()
	{
		int failures = 0;

		failures += Check ("callvirt on a static method",
		                   CallvirtCaller ("Helper", 41) (), 42);

		// More than one argument, so a site that dropped a receiver off the
		// stack would pass the wrong values rather than the wrong count.
		failures += Check ("callvirt on a static method with three arguments",
		                   CallvirtCaller ("Sum", 1, 2, 3) (), 6);

		return failures;
	}
}
