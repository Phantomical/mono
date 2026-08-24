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
 * A store from one old object to another needs the card as well, and only while a
 * concurrent collection runs. Its marker reads the heap as the mutator writes to
 * it, so a reference stored into an object the marker already read arrives through
 * the card alone. OldToOld () is the round for that, and it allocates hard enough
 * for such a collection to be under way.
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

	/* The base the old-to-old round adds, apart from the bases Main () passes. */
	const int Moved = 9000000;

	/* MonoTier::tier1 and MonoTier::tier2, as PromoteNow takes them. */
	const int tier1 = 2;
	const int tier2 = 3;

	static Holder[] fields;
	static Payload[] elements;
	static Payload[] indirect;
	static Payload one;

	/* The old-to-old round below. */
	static Holder[] targets;
	static Payload[] moved;
	static object[] survivors = new object [2];

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

	// Both ends of this store are old, so the collector reads its card only while
	// a concurrent collection runs. The array drops the payload in the same step,
	// which leaves the store as the one path to it.
	static void MoveOne (int index)
	{
		targets [index].Field = moved [index];
		moved [index] = null;
	}

	/// Allocates a block that outlives the round after it, so its objects are
	/// promoted and the collector's major allowance runs out. A concurrent
	/// collection starts on that allowance. GC.Collect () does not start one: it
	/// asks for a forced serial collection.
	static void Grow (int round)
	{
		object[] block = new object [4096];

		for (int i = 0; i < block.Length; i++)
			block [i] = new Payload (i);

		survivors [round & 1] = block;
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
		                     "StoreStatic", "MoveOne", "Grow" };

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

	/*
	 * Moves each payload from an old array into an old holder, while the
	 * allocation beside it makes the collector start concurrent collections. A
	 * concurrent collection reads the heap while this loop writes to it. A payload
	 * stored into a holder the marker already read reaches the marker through the
	 * card alone, so a card the store missed leaves that payload unmarked and the
	 * collection frees it.
	 *
	 * --gc-debug=mod-union-consistency-check reports such a card at the next
	 * collection. Without the option, the loop reports a payload whose bytes
	 * something else took.
	 */
	static bool OldToOld (string name)
	{
		targets = new Holder [Count];
		moved = new Payload [Count];

		for (int i = 0; i < Count; i++) {
			targets [i] = new Holder ();
			moved [i] = new Payload (Moved + i);
		}

		GC.Collect ();
		GC.WaitForPendingFinalizers ();
		GC.Collect ();

		for (int i = 0; i < Count; i++) {
			MoveOne (i);

			if ((i & 255) == 0)
				Grow (i >> 8);
		}

		Churn ();

		for (int i = 0; i < Count; i++) {
			if (Wrong ("old stfld", i, targets [i].Field.Value, Moved + i)) {
				Console.WriteLine ("       at {0}", name);
				return false;
			}
		}

		return true;
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
		if (!Round (1000000, "tier 0") || !OldToOld ("tier 0"))
			return 1;

		if (!Promote (tier1, "tier 1") || !Round (2000000, "tier 1")
		    || !OldToOld ("tier 1"))
			return 1;

		if (!Promote (tier2, "tier 2") || !Round (3000000, "tier 2")
		    || !OldToOld ("tier 2"))
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
