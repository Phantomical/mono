using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Every reference store the compiler writes has to mark the collector's card. A
 * card it missed leaves an old object naming a young one, and the next minor
 * collection moves that young object without repointing the field.
 *
 * Each round stores freshly allocated payloads into arrays that are already old,
 * through one opcode each, and reads them back after a minor collection. A
 * payload is reachable only through the old array, so a missed card either
 * reports the wrong number or faults on a stale pointer.
 *
 * Two things make the test decide rather than pass by luck.
 * Mono.Tiering.MonoTier::PromoteNow compiles the store methods on this thread,
 * because the interpreter has a barrier of its own and folds bodies this small
 * into their caller. And the count is large, because a handful of payloads can
 * survive on a conservatively pinned stack slot whatever the cards say.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Payload {
	public int Value;

	public Payload (int value)
	{
		Value = value;
	}
}

class Holder {
	public Payload Field;
}

class Driver {
	const int Count = 50000;

	/* MonoTier::tier1 and MonoTier::tier2, as PromoteNow takes them. */
	const int tier1 = 2;
	const int tier2 = 3;

	static Holder[] fields;
	static Payload[] elements;
	static Payload[] indirect;
	static Payload one;

	static void StoreField (Holder holder, int value)
	{
		holder.Field = new Payload (value);
	}

	static void StoreElement (Payload[] array, int index, int value)
	{
		array [index] = new Payload (value);
	}

	// ldelema, then stind.ref.
	static void StoreIndirect (ref Payload slot, int value)
	{
		slot = new Payload (value);
	}

	static void StoreStatic (int value)
	{
		one = new Payload (value);
	}

	static void Fill (int round)
	{
		for (int i = 0; i < Count; i++) {
			StoreField (fields [i], round + i);
			StoreElement (elements, i, round + i);
			StoreIndirect (ref indirect [i], round + i);
		}

		StoreStatic (round);
	}

	static void MakeTheArraysOld ()
	{
		fields = new Holder [Count];
		elements = new Payload [Count];
		indirect = new Payload [Count];

		for (int i = 0; i < Count; i++)
			fields [i] = new Holder ();

		GC.Collect ();
		GC.WaitForPendingFinalizers ();
		GC.Collect ();
	}

	static bool Promote (int tier, string name)
	{
		string[] methods = { "Fill", "StoreField", "StoreElement", "StoreIndirect",
		                     "StoreStatic" };

		foreach (string method in methods) {
			MethodInfo info = typeof (Driver).GetMethod (
				method, BindingFlags.Static | BindingFlags.NonPublic);

			if (!Mono.Tiering.MonoTier.PromoteNow (info.MethodHandle.Value, tier)) {
				Console.WriteLine ("FAIL: {0} () would not compile at {1}",
				                   method, name);
				return false;
			}
		}

		return true;
	}

	static bool Wrong (string what, int index, int got, int want)
	{
		if (got == want)
			return false;

		Console.WriteLine ("FAIL: {0} [{1}] reads {2} where {3} belongs",
		                   what, index, got, want);
		return true;
	}

	/// Fills the nursery with garbage, so that a payload the collection freed has
	/// its bytes taken by something else. A stale pointer that still reads its old
	/// contents makes the check below pass on a card the store never marked.
	static void Churn ()
	{
		object[] sink = new object [64];

		for (int i = 0; i < Count * 8; i++)
			sink [i & 63] = new Payload (-1);
	}

	static bool Round (int round, string name)
	{
		Fill (round);
		GC.Collect (0);
		Churn ();

		for (int i = 0; i < Count; i++) {
			int want = round + i;

			if (Wrong ("stfld", i, fields [i].Field.Value, want)
			    || Wrong ("stelem.ref", i, elements [i].Value, want)
			    || Wrong ("stind.ref", i, indirect [i].Value, want)) {
				Console.WriteLine ("       at {0}", name);
				return false;
			}
		}

		return !Wrong ("stsfld", 0, one.Value, round);
	}

	static int Main ()
	{
		MakeTheArraysOld ();

		// The interpreted round first, then each compiled tier. A round stores
		// over payloads the round before it promoted, so it marks cards the
		// collection in between has cleaned.
		if (!Round (1000000, "tier 0"))
			return 1;

		if (!Promote (tier1, "tier 1") || !Round (2000000, "tier 1"))
			return 1;

		if (!Promote (tier2, "tier 2") || !Round (3000000, "tier 2"))
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
