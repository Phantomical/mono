using System;
using System.Reflection;
using System.Runtime.CompilerServices;

// A value type of sixteen one-byte scalars takes a parameter register for each
// of the first six and a stack slot for the rest, so one of them as the first
// argument spends the whole integer file. The hidden return pointer goes behind
// that first argument, which leaves it in a stack slot of its own - the one
// placement mono_arch_get_call_info () could not reach before, because a value
// type used to take at most two registers.
//
// The seam is what tells the two engines' answers apart, so the tiers are
// driven by hand: -mono-tier1-threshold=0 pins every body at tier 0 and
// PromoteNow moves the one method each round is about. VretSpillCallerA is
// compiled and its callee is not, and VretSpillCalleeB is compiled and its
// caller is not, so each direction of the call is measured on its own.
namespace Mono.Tiering {
	static class MonoTier {
		[System.Runtime.CompilerServices.MethodImpl (System.Runtime.CompilerServices.MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

public class Tier0ClassicVretSpillTest
{
	const int tier1 = 3;

	struct VretSpillWide {
		public byte B0, B1, B2, B3, B4, B5, B6, B7;
		public byte B8, B9, B10, B11, B12, B13, B14, B15;
	}

	static VretSpillWide MakeWide (byte seed)
	{
		VretSpillWide w;

		w.B0 = seed; w.B1 = (byte) (seed + 1); w.B2 = (byte) (seed + 2);
		w.B3 = (byte) (seed + 3); w.B4 = (byte) (seed + 4); w.B5 = (byte) (seed + 5);
		w.B6 = (byte) (seed + 6); w.B7 = (byte) (seed + 7); w.B8 = (byte) (seed + 8);
		w.B9 = (byte) (seed + 9); w.B10 = (byte) (seed + 10); w.B11 = (byte) (seed + 11);
		w.B12 = (byte) (seed + 12); w.B13 = (byte) (seed + 13); w.B14 = (byte) (seed + 14);
		w.B15 = (byte) (seed + 15);
		return w;
	}

	// Each field is weighed by its own position, so a value that arrived in
	// the wrong slot reads as a wrong number rather than as a wrong field.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Weigh (VretSpillWide w)
	{
		return w.B0 * 1 + w.B1 * 2 + w.B2 * 3 + w.B3 * 4
			+ w.B4 * 5 + w.B5 * 6 + w.B6 * 7 + w.B7 * 8
			+ w.B8 * 9 + w.B9 * 10 + w.B10 * 11 + w.B11 * 12
			+ w.B12 * 13 + w.B13 * 14 + w.B14 * 15 + w.B15 * 16;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static VretSpillWide VretSpillCalleeA (VretSpillWide w)
	{
		return w;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static VretSpillWide VretSpillCalleeB (VretSpillWide w)
	{
		return w;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int VretSpillCallerA (byte seed)
	{
		return Weigh (VretSpillCalleeA (MakeWide (seed)));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int VretSpillCallerB (byte seed)
	{
		return Weigh (VretSpillCalleeB (MakeWide (seed)));
	}

	static bool Promote (string name)
	{
		MethodInfo method = typeof (Tier0ClassicVretSpillTest).GetMethod (
			name, BindingFlags.Static | BindingFlags.NonPublic);

		return Mono.Tiering.MonoTier.PromoteNow (method.MethodHandle.Value, tier1);
	}

	static int Check (string what, int got, int want)
	{
		if (got == want)
			return 0;

		Console.WriteLine ("FAIL: {0} weighed {1}, want {2}", what, got, want);
		return 1;
	}

	public static int Main ()
	{
		int want = 0;

		for (int i = 0; i < 16; i++)
			want += (3 + i) * (i + 1);

		int bad = 0;

		bad += Check ("tier 0 both sides", VretSpillCallerA (3), want);
		bad += Check ("tier 0 both sides", VretSpillCallerB (3), want);

		if (!Promote ("VretSpillCallerA") || !Promote ("VretSpillCalleeB")) {
			Console.WriteLine ("FAIL: a method would not compile at tier 1");
			return 1;
		}

		bad += Check ("compiled caller", VretSpillCallerA (3), want);
		bad += Check ("compiled callee", VretSpillCallerB (3), want);

		if (!Promote ("VretSpillCalleeA") || !Promote ("VretSpillCallerB")) {
			Console.WriteLine ("FAIL: a method would not compile at tier 1");
			return 1;
		}

		bad += Check ("tier 1 both sides", VretSpillCallerA (3), want);
		bad += Check ("tier 1 both sides", VretSpillCallerB (3), want);

		if (bad != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
