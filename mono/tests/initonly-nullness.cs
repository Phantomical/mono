// Verify tier-2 loads and null checks for null and non-null readonly statics.

using System;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Held {
	public static readonly object Empty = null;
	public static readonly object Filled = new object ();
}

class InitonlyNullness {
	const int tier2 = 4;

	static int fails;

	static void Fail (string message)
	{
		Console.WriteLine ("FAIL: {0}", message);
		++fails;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object ReadEmpty () { return Held.Empty; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object ReadFilled () { return Held.Filled; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool EmptyIsNull () { return Held.Empty == null; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FilledIsNull () { return Held.Filled == null; }

	static MethodInfo MethodOf (string name)
	{
		return typeof (InitonlyNullness).GetMethod (name,
			BindingFlags.Static | BindingFlags.NonPublic);
	}

	static void Promote (string name)
	{
		if (!Mono.Tiering.MonoTier.PromoteNow (MethodOf (name).MethodHandle.Value, tier2))
			Fail (String.Format ("{0} would not compile at tier 2", name));
	}

	public static int Main ()
	{
		Promote ("ReadEmpty");
		Promote ("ReadFilled");
		Promote ("EmptyIsNull");
		Promote ("FilledIsNull");

		if (ReadEmpty () != null)
			Fail ("a null-holding readonly static answered non-null");

		if (!Object.ReferenceEquals (ReadFilled (), Held.Filled))
			Fail ("a non-null readonly static answered the wrong object");

		if (!EmptyIsNull ())
			Fail ("a null check against a null-holding readonly static answered false");

		if (FilledIsNull ())
			Fail ("a null check against a non-null readonly static answered true");

		Console.WriteLine (fails == 0 ? "OK" : String.Format ("{0} failure(s)", fails));
		return fails == 0 ? 0 : 1;
	}
}
