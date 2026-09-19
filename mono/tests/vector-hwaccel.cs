using System;
using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Tier 0 substitutes an intrinsic for Vector.IsHardwareAccelerated, while
 * tiers 1 and 2 compile the managed getter. Verify that all tiers return true.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Program {
	const int Tier1 = 3;
	const int Tier2 = 4;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool Accelerated ()
	{
		return Vector.IsHardwareAccelerated;
	}

	static bool Promote (int tier)
	{
		MethodInfo target = typeof (Program).GetMethod (
			"Accelerated", BindingFlags.Static | BindingFlags.NonPublic);

		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: Accelerated () would not promote to tier {0}", tier);
		return false;
	}

	static bool Check (string tier)
	{
		if (Accelerated ())
			return true;

		Console.WriteLine ("FAIL: {0} answered false", tier);
		return false;
	}

	public static int Main ()
	{
		if (!Check ("tier 0"))
			return 1;

		if (!Promote (Tier1) || !Check ("tier 1"))
			return 1;

		if (!Promote (Tier2) || !Check ("tier 2"))
			return 1;

		return 0;
	}
}
