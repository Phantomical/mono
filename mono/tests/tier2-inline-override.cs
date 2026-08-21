using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * A method replaced after tier 2 folded a copy of it into a caller.
 *
 * The copy sits under no thunk, so pointing the method's entry at the
 * replacement does not reach it: the caller would go on running the body it was
 * compiled with. Each method's record names the callers that folded it in, and
 * an override takes their entries back to the tier they ran at before.
 *
 * Both inliners are covered. Small () is a shape the pre-pass takes without
 * weighing it, and Branchy () is one only the cost model behind it can.
 */

namespace Mono.Overrides {

	/* The runtime registers this as it starts, whatever declares it. */
	public static class MonoOverride {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern void Install (IntPtr target, IntPtr replacement);
	}
}

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

public static class Folded {
	public static int Small (int x)
	{
		return x + 1;
	}

	public static int Branchy (int x)
	{
		if (x < 0)
			return -x;

		return x > 100 ? x - 100 : x + 2;
	}
}

public static class Replacements {
	public static int Small (int x)
	{
		return x + 1000;
	}

	public static int Branchy (int x)
	{
		return x + 2000;
	}
}

public static class Program {
	static int fails;

	static void Check (string what, long got, long want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL: {0}: got {1}, want {2}", what, got, want);
		++fails;
	}

	static int Root (int x)
	{
		return Folded.Small (x) + Folded.Branchy (x);
	}

	static MethodInfo MethodOf (Type type, string name)
	{
		return type.GetMethod (name, BindingFlags.Public | BindingFlags.Static);
	}

	static bool Promote (MethodInfo method, int tier, string what)
	{
		if (Mono.Tiering.MonoTier.PromoteNow (method.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: {0} would not compile at tier {1}", what, tier);
		++fails;
		return false;
	}

	public static int Main ()
	{
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!Promote (root, 2, "Root ()"))
			return 1;

		for (int i = 0; i < 2000; ++i)
			Root (7);

		if (!Promote (root, 3, "Root ()"))
			return 1;

		Check ("the folded answer", Root (7), 8 + 9);

		Mono.Overrides.MonoOverride.Install (
			MethodOf (typeof (Folded), "Small").MethodHandle.Value,
			MethodOf (typeof (Replacements), "Small").MethodHandle.Value);

		Check ("the replacement reaches a body that folded the method in",
			Root (7), 1007 + 9);

		Mono.Overrides.MonoOverride.Install (
			MethodOf (typeof (Folded), "Branchy").MethodHandle.Value,
			MethodOf (typeof (Replacements), "Branchy").MethodHandle.Value);

		Check ("and so does one that only the cost model folded",
			Root (7), 1007 + 2007);

		// Whatever tier the record went back to, the answers stay the ones the
		// replacements give. One report however many calls disagree.
		int wrong = 0;

		for (int i = 0; i < 2000; ++i)
			if (Root (7) != 1007 + 2007)
				++wrong;

		Check ("the calls that answered as they did before tier 2", wrong, 0);

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
