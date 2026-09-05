using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

// An explicit layout can put several fields at one offset, and neither engine
// can state an overlap: each keeps the first field at an offset and calls the
// rest of it layout no field claims. "First" has to mean the same thing on both
// sides, because the field that keeps a slot is what decides which register
// file the bytes travel in. UnionAbiRegister is the shape that tells them
// apart - System.Numerics.Register's own field set, where an unstable sort by
// offset does not leave byte_0 in front of the double at the same offset.
namespace Mono.Tiering {
	static class MonoTier {
		[System.Runtime.CompilerServices.MethodImpl (System.Runtime.CompilerServices.MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

[StructLayout (LayoutKind.Explicit)]
public struct UnionAbiRegister
{
	[FieldOffset (0)] public Byte byte_0;
	[FieldOffset (1)] public Byte byte_1;
	[FieldOffset (2)] public Byte byte_2;
	[FieldOffset (3)] public Byte byte_3;
	[FieldOffset (4)] public Byte byte_4;
	[FieldOffset (5)] public Byte byte_5;
	[FieldOffset (6)] public Byte byte_6;
	[FieldOffset (7)] public Byte byte_7;
	[FieldOffset (8)] public Byte byte_8;
	[FieldOffset (9)] public Byte byte_9;
	[FieldOffset (10)] public Byte byte_10;
	[FieldOffset (11)] public Byte byte_11;
	[FieldOffset (12)] public Byte byte_12;
	[FieldOffset (13)] public Byte byte_13;
	[FieldOffset (14)] public Byte byte_14;
	[FieldOffset (15)] public Byte byte_15;

	[FieldOffset (0)] public SByte sbyte_0;
	[FieldOffset (1)] public SByte sbyte_1;
	[FieldOffset (2)] public SByte sbyte_2;
	[FieldOffset (3)] public SByte sbyte_3;
	[FieldOffset (4)] public SByte sbyte_4;
	[FieldOffset (5)] public SByte sbyte_5;
	[FieldOffset (6)] public SByte sbyte_6;
	[FieldOffset (7)] public SByte sbyte_7;
	[FieldOffset (8)] public SByte sbyte_8;
	[FieldOffset (9)] public SByte sbyte_9;
	[FieldOffset (10)] public SByte sbyte_10;
	[FieldOffset (11)] public SByte sbyte_11;
	[FieldOffset (12)] public SByte sbyte_12;
	[FieldOffset (13)] public SByte sbyte_13;
	[FieldOffset (14)] public SByte sbyte_14;
	[FieldOffset (15)] public SByte sbyte_15;

	[FieldOffset (0)] public UInt16 uint16_0;
	[FieldOffset (2)] public UInt16 uint16_1;
	[FieldOffset (4)] public UInt16 uint16_2;
	[FieldOffset (6)] public UInt16 uint16_3;
	[FieldOffset (8)] public UInt16 uint16_4;
	[FieldOffset (10)] public UInt16 uint16_5;
	[FieldOffset (12)] public UInt16 uint16_6;
	[FieldOffset (14)] public UInt16 uint16_7;

	[FieldOffset (0)] public Int16 int16_0;
	[FieldOffset (2)] public Int16 int16_1;
	[FieldOffset (4)] public Int16 int16_2;
	[FieldOffset (6)] public Int16 int16_3;
	[FieldOffset (8)] public Int16 int16_4;
	[FieldOffset (10)] public Int16 int16_5;
	[FieldOffset (12)] public Int16 int16_6;
	[FieldOffset (14)] public Int16 int16_7;

	[FieldOffset (0)] public UInt32 uint32_0;
	[FieldOffset (4)] public UInt32 uint32_1;
	[FieldOffset (8)] public UInt32 uint32_2;
	[FieldOffset (12)] public UInt32 uint32_3;

	[FieldOffset (0)] public Int32 int32_0;
	[FieldOffset (4)] public Int32 int32_1;
	[FieldOffset (8)] public Int32 int32_2;
	[FieldOffset (12)] public Int32 int32_3;

	[FieldOffset (0)] public UInt64 uint64_0;
	[FieldOffset (8)] public UInt64 uint64_1;

	[FieldOffset (0)] public Int64 int64_0;
	[FieldOffset (8)] public Int64 int64_1;

	[FieldOffset (0)] public Single single_0;
	[FieldOffset (4)] public Single single_1;
	[FieldOffset (8)] public Single single_2;
	[FieldOffset (12)] public Single single_3;

	[FieldOffset (0)] public Double double_0;
	[FieldOffset (8)] public Double double_1;
}

// The union behind a field of its own, which is how System.Numerics.Vector<T>
// carries it.
public struct UnionAbiHolder
{
	public UnionAbiRegister register;
}

public class Tier0ClassicUnionAbiTest
{
	const int tier1 = 3;

	static UnionAbiRegister MakeRegister (byte seed)
	{
		UnionAbiRegister r = new UnionAbiRegister ();

		r.byte_0 = seed; r.byte_1 = (byte) (seed + 1); r.byte_2 = (byte) (seed + 2);
		r.byte_3 = (byte) (seed + 3); r.byte_4 = (byte) (seed + 4);
		r.byte_5 = (byte) (seed + 5); r.byte_6 = (byte) (seed + 6);
		r.byte_7 = (byte) (seed + 7); r.byte_8 = (byte) (seed + 8);
		r.byte_9 = (byte) (seed + 9); r.byte_10 = (byte) (seed + 10);
		r.byte_11 = (byte) (seed + 11); r.byte_12 = (byte) (seed + 12);
		r.byte_13 = (byte) (seed + 13); r.byte_14 = (byte) (seed + 14);
		r.byte_15 = (byte) (seed + 15);
		return r;
	}

	// Each byte is weighed by its own position, so a value that arrived in the
	// wrong slot reads as a wrong number rather than as a wrong field.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UnionAbiWeighA (UnionAbiRegister r)
	{
		return r.byte_0 * 1 + r.byte_1 * 2 + r.byte_2 * 3 + r.byte_3 * 4
			+ r.byte_4 * 5 + r.byte_5 * 6 + r.byte_6 * 7 + r.byte_7 * 8
			+ r.byte_8 * 9 + r.byte_9 * 10 + r.byte_10 * 11 + r.byte_11 * 12
			+ r.byte_12 * 13 + r.byte_13 * 14 + r.byte_14 * 15 + r.byte_15 * 16;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UnionAbiWeighB (UnionAbiHolder h)
	{
		UnionAbiRegister r = h.register;

		return r.byte_0 * 1 + r.byte_1 * 2 + r.byte_2 * 3 + r.byte_3 * 4
			+ r.byte_4 * 5 + r.byte_5 * 6 + r.byte_6 * 7 + r.byte_7 * 8
			+ r.byte_8 * 9 + r.byte_9 * 10 + r.byte_10 * 11 + r.byte_11 * 12
			+ r.byte_12 * 13 + r.byte_13 * 14 + r.byte_14 * 15 + r.byte_15 * 16;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UnionAbiCallA (byte seed)
	{
		return UnionAbiWeighA (MakeRegister (seed));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UnionAbiCallB (byte seed)
	{
		UnionAbiHolder h = new UnionAbiHolder ();

		h.register = MakeRegister (seed);
		return UnionAbiWeighB (h);
	}

	static bool Promote (string name)
	{
		MethodInfo method = typeof (Tier0ClassicUnionAbiTest).GetMethod (
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

		bad += Check ("tier 0 union", UnionAbiCallA (3), want);
		bad += Check ("tier 0 holder", UnionAbiCallB (3), want);

		if (!Promote ("UnionAbiWeighA") || !Promote ("UnionAbiWeighB")) {
			Console.WriteLine ("FAIL: a method would not compile at tier 1");
			return 1;
		}

		bad += Check ("compiled union callee", UnionAbiCallA (3), want);
		bad += Check ("compiled holder callee", UnionAbiCallB (3), want);

		if (!Promote ("UnionAbiCallA") || !Promote ("UnionAbiCallB")) {
			Console.WriteLine ("FAIL: a method would not compile at tier 1");
			return 1;
		}

		bad += Check ("tier 1 union", UnionAbiCallA (3), want);
		bad += Check ("tier 1 holder", UnionAbiCallB (3), want);

		if (bad != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
