using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * The class an object reference has where two paths into a block leave it in
 * one evaluation stack slot. ECMA-335 III.1.8.1.3 makes that the closest
 * common supertype, and the translator reads a slot's class as an upper bound
 * on what is in it: a sealed one settles a virtual call at translate time. So a
 * slot that keeps the first path's class calls that path's override on the
 * other path's object.
 *
 * Each check here is a number rather than a crash. What a wrong call did in the
 * wild is write a field past the end of the shorter of the two classes, which
 * is why Wide is longer than Narrow.
 *
 * Mono.Tiering.MonoTier::PromoteNow compiles a method at the tier it is given,
 * on this thread, so the test needs no environment and races no compile worker.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

abstract class Shape {
	public abstract int Which ();
}

sealed class Narrow : Shape {
	long a;

	public override int Which () { return (int)(a + 1); }
}

sealed class Wide : Shape {
	long a, b, c, d;

	public override int Which () { return (int)(a + b + c + d + 2); }
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

	/* The receiver is what the two newobj arms left in one slot. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int WhichOf (bool narrow)
	{
		return (narrow ? (Shape)new Narrow () : (Shape)new Wide ()).Which ();
	}

	/* Three paths into the slot, two of which agree. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int WhichOfThree (int pick)
	{
		return (pick == 0 ? (Shape)new Narrow ()
			: pick == 1 ? (Shape)new Wide ()
			: (Shape)new Narrow ()).Which ();
	}

	static void CheckAll (string tier)
	{
		Check (tier + ": the narrow arm", WhichOf (true), 1);
		Check (tier + ": the wide arm", WhichOf (false), 2);
		Check (tier + ": three paths, first", WhichOfThree (0), 1);
		Check (tier + ": three paths, second", WhichOfThree (1), 2);
		Check (tier + ": three paths, third", WhichOfThree (2), 1);
	}

	static bool Promote (string name, int tier)
	{
		MethodInfo method = typeof (Program).GetMethod (name,
			BindingFlags.Static | BindingFlags.NonPublic);

		if (Mono.Tiering.MonoTier.PromoteNow (method.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: {0} () would not compile at tier {1}", name, tier);
		++fails;
		return false;
	}

	static bool PromoteAll (int tier)
	{
		return Promote ("WhichOf", tier) & Promote ("WhichOfThree", tier);
	}

	public static int Main ()
	{
		CheckAll ("first call");

		if (!PromoteAll (2))
			return 1;
		CheckAll ("tier 1");

		if (!PromoteAll (3))
			return 1;
		CheckAll ("tier 2");

		return fails == 0 ? 0 : 1;
	}
}
