// The corpus test-method-override.cpp overrides methods in.
//
// Every target and its replacement answer differently, so the answer says which
// body ran. A caller of each target is here as well, since what an interpreted
// caller does is the half a direct call cannot show.

using System.Runtime.CompilerServices;

public class Override {

	// ------------------------------------------------------------ plain
	public static int Target (int x) { return x + 1; }
	public static int Replacement (int x) { return x + 1000; }
	public static int SecondReplacement (int x) { return x + 2000; }

	public static int CallTarget (int x) { return Target (x); }

	// A second target, so a case that needs an untouched method has one: an
	// override is never taken back.
	public static int Retargeted (int x) { return x + 1; }
	public static int CallRetargeted (int x) { return Retargeted (x); }

	// ---------------------------------------------------------- generic
	//
	// mono_interp_jit_call_marshallable () refuses an inflated method, so a
	// detour to native code never reaches these from an interpreted caller.
	public static T Generic<T> (T v) { return v; }
	public static T GenericReplacement<T> (T v) { return default (T); }

	public static int CallGeneric (int x) { return Generic<int> (x); }

	// ------------------------------------------------------------- wide
	//
	// Eight parameters, where the same predicate refuses more than six.
	public static int Wide (int a, int b, int c, int d, int e, int f, int g, int h)
	{
		return a + b + c + d + e + f + g + h;
	}

	public static int WideReplacement (int a, int b, int c, int d, int e, int f, int g, int h)
	{
		return 1000;
	}

	public static int CallWide (int x) { return Wide (x, 0, 0, 0, 0, 0, 0, 0); }

	// ---------------------------------------------------------- inlined
	//
	// Small enough for the interpreter to copy into its caller. Overriding it
	// marks it NoInlining, so a caller transformed afterwards calls it instead.
	public static int Small (int x) { return x + 1; }
	public static int CallSmall (int x) { return Small (x); }

	public static int Main ()
	{
		return 0;
	}
}
