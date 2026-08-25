/*
 * The shapes a `!tbaa` tag on a managed access has to survive.
 *
 * The back end tags the typed field and element opcodes - ldfld, stfld, ldsfld,
 * stsfld, ldelem, stelem and the array accessors - splitting managed memory
 * into a reference leaf and a scalar tree (ManagedAccess,
 * mono/llvm/method-to-llvm.hpp). The cases below reach one slot under two
 * types. A wrong split turns that into a dropped store, not a slow method.
 *
 * Every case runs at all three tiers. A tag rides the IR the translator writes,
 * so tier 1 and tier 2 both carry it, and only tier 2 optimizes hard enough to
 * act on one.
 */

using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Threading;

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

/// A union of two scalars, which II.10.7 permits and mono loads. Both fields
/// are on the scalar leaf, so the two accesses have to keep aliasing.
[StructLayout (LayoutKind.Explicit)]
struct ScalarUnion {
	[FieldOffset (0)] public double AsDouble;
	[FieldOffset (0)] public long AsLong;
}

class Holder {
	public int Counter;
	public object Reference;
	public double Value;
}

struct WithReference {
	public object Tag;
	public int Number;
}

static class Program {
	static int fails;

	static void Check (string what, long got, long want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL: {0} is {1}, wanted {2}", what, got, want);
		++fails;
	}

	static void Check (string what, object got, object want)
	{
		if (ReferenceEquals (got, want))
			return;

		Console.WriteLine ("FAIL: {0} is {1}, wanted {2}", what, got, want);
		++fails;
	}

	/*
	 * A scalar union. The long store and the double load are stfld and ldfld
	 * on two fields at one offset, so a per-field or per-width split would
	 * call them disjoint and hand back the stale double.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static double UnionRoundTrip (long bits)
	{
		ScalarUnion u = new ScalarUnion ();

		u.AsLong = bits;
		return u.AsDouble;
	}

	/*
	 * Volatile.Write is `Unsafe.As<int, VolatileInt32> (ref location).Value =
	 * value`, so the write is stfld through a class the storage is not an
	 * instance of, while the read below is ldfld through the real one. Both
	 * are on the scalar leaf.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int VolatileScalar (Holder h, int value)
	{
		Volatile.Write (ref h.Counter, value);
		return h.Counter;
	}

	/// The same trick for a reference slot, which goes through VolatileObject.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static object VolatileReference (Holder h, object value)
	{
		Volatile.Write (ref h.Reference, value);
		return h.Reference;
	}

	/*
	 * A store of a double into an object field must not move across a load of
	 * a reference field. This is the shape the tag exists for, so it is the
	 * case a too-wide split gets wrong in the other direction: the reference
	 * load is hoisted and the store lands somewhere else.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static double ScalarStoreKeepsReference (Holder h, Holder other)
	{
		h.Reference = other;
		h.Value = 1.0;
		other.Value = 2.0;
		return ((Holder) h.Reference).Value;
	}

	/*
	 * Array covariance. o and s name one array, so the stelem.ref through o
	 * and the ldelem through s reach one slot. I.8.7.1 rule 5.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string Covariant (string value)
	{
		string[] s = new string [1];
		object[] o = s;

		o [0] = value;
		return s [0];
	}

	/*
	 * array-element-compatible-with is "agnostic with respect to enumerations
	 * and integral signed-ness" (I.8.7.1), so these three name one array and
	 * one slot.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int EnumAliasesInt (int value)
	{
		int[] ints = new int [1];
		DayOfWeek[] days = (DayOfWeek[]) (object) ints;

		days [0] = (DayOfWeek) value;
		return ints [0];
	}

	/*
	 * A block copy is the InternalBlockCopy icall, so the bytes move in code
	 * LLVM cannot see. The tagged stelem before it and the tagged ldelem after
	 * it must not move across that call.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int BlockCopy (int value)
	{
		int[] source = new int [4];
		int[] target = new int [4];

		source [2] = value;
		Buffer.BlockCopy (source, 0, target, 0, 16);
		return target [2];
	}

	/// A struct holding a reference copied as bytes, which crosses both leaves.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static WithReference CopyStruct (object tag, int number)
	{
		WithReference one = new WithReference ();
		WithReference two;

		one.Tag = tag;
		one.Number = number;
		two = one;
		return two;
	}

	/*
	 * GCHandle.Target reads a strong handle with `Unsafe.As<IntPtr, object>
	 * (ref *(IntPtr*) handle)`, which reads a reference out of storage the
	 * runtime writes as an IntPtr. It arrives through ldind.ref, so it stays
	 * untagged.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static object HandleRoundTrip (object value)
	{
		GCHandle handle = GCHandle.Alloc (value);

		try {
			return handle.Target;
		} finally {
			handle.Free ();
		}
	}

	/// A reference array copied by the FastCopy icall, between two tagged
	/// element accesses.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static object RawReferenceArray (object value)
	{
		object[] array = new object [2];

		array [1] = value;
		Array.Copy (array, 0, array, 0, 2);
		return array [1];
	}

	static void RunAll (string tier)
	{
		Holder h = new Holder ();
		Holder other = new Holder ();
		object marker = new object ();

		Check (tier + " union", BitConverter.DoubleToInt64Bits (UnionRoundTrip (0x4010000000000000L)),
			0x4010000000000000L);
		Check (tier + " volatile scalar", VolatileScalar (h, 7), 7);
		Check (tier + " volatile reference", VolatileReference (h, marker), marker);
		Check (tier + " scalar store keeps reference",
			BitConverter.DoubleToInt64Bits (ScalarStoreKeepsReference (h, other)),
			BitConverter.DoubleToInt64Bits (2.0));
		Check (tier + " covariant", Covariant ("x"), "x");
		Check (tier + " enum aliases int", EnumAliasesInt (3), 3);
		Check (tier + " block copy", BlockCopy (11), 11);

		WithReference copied = CopyStruct (marker, 5);

		Check (tier + " struct copy tag", copied.Tag, marker);
		Check (tier + " struct copy number", copied.Number, 5);
		Check (tier + " gchandle", HandleRoundTrip (marker), marker);
		Check (tier + " raw reference array", RawReferenceArray (marker), marker);
	}

	static bool Promote (string name, int tier)
	{
		MethodInfo method = typeof (Program).GetMethod (name,
			BindingFlags.Static | BindingFlags.NonPublic);

		if (Mono.Tiering.MonoTier.PromoteNow (method.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: {0} would not compile at tier {1}", name, tier);
		++fails;
		return false;
	}

	static readonly string[] cases = {
		"UnionRoundTrip", "VolatileScalar", "VolatileReference",
		"ScalarStoreKeepsReference", "Covariant", "EnumAliasesInt", "BlockCopy",
		"CopyStruct", "HandleRoundTrip", "RawReferenceArray",
	};

	public static int Main ()
	{
		RunAll ("tier0");

		// MonoTier::tier1 is 2 and tier2 is 3 (mono/mini/domain-method.hpp).
		foreach (string name in cases)
			Promote (name, 2);
		RunAll ("tier1");

		foreach (string name in cases)
			Promote (name, 3);
		RunAll ("tier2");

		Console.WriteLine (fails == 0 ? "OK" : fails + " failures");
		return fails == 0 ? 0 : 1;
	}
}
