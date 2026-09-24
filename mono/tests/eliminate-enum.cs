using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Runtime.Serialization.Formatters.Binary;
using System.Runtime.CompilerServices;

/*
 * System.Enum's operations over every underlying type C# can declare: hashing,
 * equality and ordering, the default comparers, the underlying type, boxing a
 * value through ToObject (), and reading the value back. Each probe runs at
 * tier 0, then at tier 1 and tier 2, and every tier has to give the answer the
 * underlying primitive gives. Each underlying type's extremes are there
 * because a load or a compare of the wrong width or signedness gets exactly
 * those wrong.
 */

enum S8 : sbyte { Min = sbyte.MinValue, Neg = -1, Zero = 0, Max = sbyte.MaxValue }
enum U8 : byte { Zero = 0, Mid = 0x7f, High = 0x80, Max = byte.MaxValue }
enum S16 : short { Min = short.MinValue, Neg = -1, Zero = 0, Max = short.MaxValue }
enum U16 : ushort { Zero = 0, Mid = 0x7fff, High = 0x8000, Max = ushort.MaxValue }
enum S32 { Min = int.MinValue, Neg = -1, Zero = 0, Max = int.MaxValue }
enum U32 : uint { Zero = 0, Mid = 0x7fffffff, High = 0x80000000, Max = uint.MaxValue }
enum S64 : long { Min = long.MinValue, Neg = -1, Zero = 0, Max = long.MaxValue }
enum U64 : ulong { Zero = 0, Mid = 0x7fffffffffffffff, High = 0x8000000000000000, Max = ulong.MaxValue }
enum Empty { }
enum Aliased { One = 1, Uno = 1, Three = 3 }
enum Run { A, B, C, D }
enum RunThroughZero : sbyte { MinusTwo = -2, MinusOne = -1, Zero = 0, One = 1 }
enum Runs32 {
	R0 = 0, R1 = 2, R2 = 4, R3 = 6, R4 = 8, R5 = 10, R6 = 12, R7 = 14, R8 = 16, R9 = 18,
	R10 = 20, R11 = 22, R12 = 24, R13 = 26, R14 = 28, R15 = 30, R16 = 32, R17 = 34, R18 = 36, R19 = 38,
	R20 = 40, R21 = 42, R22 = 44, R23 = 46, R24 = 48, R25 = 50, R26 = 52, R27 = 54, R28 = 56, R29 = 58,
	R30 = 60, R31 = 62, R31b = 63
}
enum Runs33 {
	R0 = 0, R1 = 2, R2 = 4, R3 = 6, R4 = 8, R5 = 10, R6 = 12, R7 = 14, R8 = 16, R9 = 18,
	R10 = 20, R11 = 22, R12 = 24, R13 = 26, R14 = 28, R15 = 30, R16 = 32, R17 = 34, R18 = 36, R19 = 38,
	R20 = 40, R21 = 42, R22 = 44, R23 = 46, R24 = 48, R25 = 50, R26 = 52, R27 = 54, R28 = 56, R29 = 58,
	R30 = 60, R31 = 62, R32 = 64
}
enum RunThroughMax : ulong { Zero = 0, MaxMinusOne = ulong.MaxValue - 1, Max = ulong.MaxValue }

class Program {
	const int tier1 = 3;
	const int tier2 = 4;

	[MethodImpl (MethodImplOptions.NoInlining)] static int HashS8 (S8 v) => v.GetHashCode ();
	[MethodImpl (MethodImplOptions.NoInlining)] static int HashU8 (U8 v) => v.GetHashCode ();
	[MethodImpl (MethodImplOptions.NoInlining)] static int HashS16 (S16 v) => v.GetHashCode ();
	[MethodImpl (MethodImplOptions.NoInlining)] static int HashU16 (U16 v) => v.GetHashCode ();
	[MethodImpl (MethodImplOptions.NoInlining)] static int HashS32 (S32 v) => v.GetHashCode ();
	[MethodImpl (MethodImplOptions.NoInlining)] static int HashU32 (U32 v) => v.GetHashCode ();
	[MethodImpl (MethodImplOptions.NoInlining)] static int HashS64 (S64 v) => v.GetHashCode ();
	[MethodImpl (MethodImplOptions.NoInlining)] static int HashU64 (U64 v) => v.GetHashCode ();

	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpS8 (S8 a, S8 b) => a.CompareTo (b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpU8 (U8 a, U8 b) => a.CompareTo (b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpS16 (S16 a, S16 b) => a.CompareTo (b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpU16 (U16 a, U16 b) => a.CompareTo (b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpS32 (S32 a, S32 b) => a.CompareTo (b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpU32 (U32 a, U32 b) => a.CompareTo (b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpS64 (S64 a, S64 b) => a.CompareTo (b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpU64 (U64 a, U64 b) => a.CompareTo (b);

	// The constrained. callvirt a generic body writes, rather than the box and
	// the call to Enum.CompareTo () C# writes above.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CmpConstrained<T> (T a, T b) where T : IComparable => a.CompareTo (b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpConstrainedU32 (U32 a, U32 b) => CmpConstrained (a, b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpConstrainedS64 (S64 a, S64 b) => CmpConstrained (a, b);

	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpDefaultS8 (S8 a, S8 b) => Comparer<S8>.Default.Compare (a, b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpDefaultU8 (U8 a, U8 b) => Comparer<U8>.Default.Compare (a, b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpDefaultS16 (S16 a, S16 b) => Comparer<S16>.Default.Compare (a, b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpDefaultU16 (U16 a, U16 b) => Comparer<U16>.Default.Compare (a, b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpDefaultS32 (S32 a, S32 b) => Comparer<S32>.Default.Compare (a, b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpDefaultU32 (U32 a, U32 b) => Comparer<U32>.Default.Compare (a, b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpDefaultS64 (S64 a, S64 b) => Comparer<S64>.Default.Compare (a, b);
	[MethodImpl (MethodImplOptions.NoInlining)] static int CmpDefaultU64 (U64 a, U64 b) => Comparer<U64>.Default.Compare (a, b);

	[MethodImpl (MethodImplOptions.NoInlining)] static bool EqS8 (S8 a, S8 b) => a.Equals (b);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool EqU16 (U16 a, U16 b) => a.Equals (b);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool EqU32 (U32 a, U32 b) => a.Equals (b);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool EqS64 (S64 a, S64 b) => a.Equals (b);

	// other's class is not settled here, so the elimination has to test it.
	[MethodImpl (MethodImplOptions.NoInlining)] static bool EqObjectS32 (S32 a, object b) => a.Equals (b);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool EqObjectU8 (U8 a, object b) => a.Equals (b);

	[MethodImpl (MethodImplOptions.NoInlining)] static bool EqDefaultU64 (U64 a, U64 b) => EqualityComparer<U64>.Default.Equals (a, b);

	[MethodImpl (MethodImplOptions.NoInlining)] static Type TypeUnderlyingU16 () => Enum.GetUnderlyingType (typeof (U16));
	[MethodImpl (MethodImplOptions.NoInlining)] static TypeCode TypeCodeS64 (S64 v) => v.GetTypeCode ();
	[MethodImpl (MethodImplOptions.NoInlining)] static bool TypeIsEnumU8 () => typeof (U8).IsEnum;
	[MethodImpl (MethodImplOptions.NoInlining)] static bool TypeIsEnumInt () => typeof (int).IsEnum;

	[MethodImpl (MethodImplOptions.NoInlining)] static object BoxS8 (int v) => Enum.ToObject (typeof (S8), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static object BoxU8 (int v) => Enum.ToObject (typeof (U8), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static object BoxU64 (ulong v) => Enum.ToObject (typeof (U64), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static object BoxNotEnum (int v) => Enum.ToObject (typeof (int), v);

	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedS8 (S8 v) => Enum.IsDefined (typeof (S8), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedU64 (U64 v) => Enum.IsDefined (typeof (U64), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedU8Byte (byte v) => Enum.IsDefined (typeof (U8), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedS16Int (int v) => Enum.IsDefined (typeof (S16), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedS32OtherEnum (S16 v) => Enum.IsDefined (typeof (S32), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedEmpty (Empty v) => Enum.IsDefined (typeof (Empty), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedAliased (Aliased v) => Enum.IsDefined (typeof (Aliased), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedRun (Run v) => Enum.IsDefined (typeof (Run), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedRunThroughZero (RunThroughZero v) => Enum.IsDefined (typeof (RunThroughZero), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedRunThroughMax (RunThroughMax v) => Enum.IsDefined (typeof (RunThroughMax), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedRuns32 (Runs32 v) => Enum.IsDefined (typeof (Runs32), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedRuns33 (Runs33 v) => Enum.IsDefined (typeof (Runs33), v);
	[MethodImpl (MethodImplOptions.NoInlining)] static bool DefinedName (string v) => Enum.IsDefined (typeof (S8), v);

	[MethodImpl (MethodImplOptions.NoInlining)] static long ValueS32 (S32 v) => Convert.ToInt64 (v);
	[MethodImpl (MethodImplOptions.NoInlining)] static string ValueU32 (U32 v) => v.ToString ("D");
	[MethodImpl (MethodImplOptions.NoInlining)] static int ValueIConvertibleS16 (S16 v) => ((IConvertible) v).ToInt32 (null);
	[MethodImpl (MethodImplOptions.NoInlining)] static string ValueNameU8 (U8 v) => v.ToString ();

	static int failures;

	static void Check<T> (string tier, string what, T got, T want)
	{
		if (EqualityComparer<T>.Default.Equals (got, want))
			return;

		Console.WriteLine ("FAIL: {0} {1} gave {2}, wanted {3}", tier, what, got, want);
		failures++;
	}

	static int Order (long a, long b) => a < b ? -1 : a > b ? 1 : 0;
	static int Order (ulong a, ulong b) => a < b ? -1 : a > b ? 1 : 0;

	static void RunAll (string tier)
	{
		foreach (S8 a in Enum.GetValues (typeof (S8))) {
			Check (tier, "HashS8 " + a, HashS8 (a), ((sbyte) a).GetHashCode ());
			foreach (S8 b in Enum.GetValues (typeof (S8))) {
				int want = Order ((sbyte) a, (sbyte) b);
				Check (tier, "CmpS8 " + a + " " + b, CmpS8 (a, b), want);
				Check (tier, "CmpDefaultS8 " + a + " " + b, (CmpDefaultS8 (a, b)), want);
				Check (tier, "EqS8 " + a + " " + b, EqS8 (a, b), (sbyte) a == (sbyte) b);
			}
		}

		foreach (U8 a in Enum.GetValues (typeof (U8))) {
			Check (tier, "HashU8 " + a, HashU8 (a), ((byte) a).GetHashCode ());
			foreach (U8 b in Enum.GetValues (typeof (U8))) {
				int want = Order ((byte) a, (byte) b);
				Check (tier, "CmpU8 " + a + " " + b, CmpU8 (a, b), want);
				Check (tier, "CmpDefaultU8 " + a + " " + b, (CmpDefaultU8 (a, b)), want);
			}
		}

		foreach (S16 a in Enum.GetValues (typeof (S16))) {
			Check (tier, "HashS16 " + a, HashS16 (a), ((short) a).GetHashCode ());
			foreach (S16 b in Enum.GetValues (typeof (S16))) {
				int want = Order ((short) a, (short) b);
				Check (tier, "CmpS16 " + a + " " + b, CmpS16 (a, b), want);
				Check (tier, "CmpDefaultS16 " + a + " " + b, (CmpDefaultS16 (a, b)), want);
			}
		}

		foreach (U16 a in Enum.GetValues (typeof (U16))) {
			Check (tier, "HashU16 " + a, HashU16 (a), ((ushort) a).GetHashCode ());
			foreach (U16 b in Enum.GetValues (typeof (U16))) {
				int want = Order ((ushort) a, (ushort) b);
				Check (tier, "CmpU16 " + a + " " + b, CmpU16 (a, b), want);
				Check (tier, "CmpDefaultU16 " + a + " " + b, (CmpDefaultU16 (a, b)), want);
				Check (tier, "EqU16 " + a + " " + b, EqU16 (a, b), (ushort) a == (ushort) b);
			}
		}

		foreach (S32 a in Enum.GetValues (typeof (S32))) {
			Check (tier, "HashS32 " + a, HashS32 (a), ((int) a).GetHashCode ());
			foreach (S32 b in Enum.GetValues (typeof (S32))) {
				int want = Order ((int) a, (int) b);
				Check (tier, "CmpS32 " + a + " " + b, CmpS32 (a, b), want);
				Check (tier, "CmpDefaultS32 " + a + " " + b, (CmpDefaultS32 (a, b)), want);
			}
		}

		foreach (U32 a in Enum.GetValues (typeof (U32))) {
			Check (tier, "HashU32 " + a, HashU32 (a), ((uint) a).GetHashCode ());
			foreach (U32 b in Enum.GetValues (typeof (U32))) {
				int want = Order ((uint) a, (uint) b);
				Check (tier, "CmpU32 " + a + " " + b, CmpU32 (a, b), want);
				Check (tier, "CmpConstrainedU32 " + a + " " + b, CmpConstrainedU32 (a, b), want);
				Check (tier, "CmpDefaultU32 " + a + " " + b, (CmpDefaultU32 (a, b)), want);
				Check (tier, "EqU32 " + a + " " + b, EqU32 (a, b), (uint) a == (uint) b);
			}
		}

		foreach (S64 a in Enum.GetValues (typeof (S64))) {
			Check (tier, "HashS64 " + a, HashS64 (a), ((long) a).GetHashCode ());
			foreach (S64 b in Enum.GetValues (typeof (S64))) {
				int want = Order ((long) a, (long) b);
				Check (tier, "CmpS64 " + a + " " + b, CmpS64 (a, b), want);
				Check (tier, "CmpConstrainedS64 " + a + " " + b, CmpConstrainedS64 (a, b), want);
				Check (tier, "CmpDefaultS64 " + a + " " + b, (CmpDefaultS64 (a, b)), want);
				Check (tier, "EqS64 " + a + " " + b, EqS64 (a, b), (long) a == (long) b);
			}
		}

		foreach (U64 a in Enum.GetValues (typeof (U64))) {
			Check (tier, "HashU64 " + a, HashU64 (a), ((ulong) a).GetHashCode ());
			foreach (U64 b in Enum.GetValues (typeof (U64))) {
				int want = Order ((ulong) a, (ulong) b);
				Check (tier, "CmpU64 " + a + " " + b, CmpU64 (a, b), want);
				Check (tier, "CmpDefaultU64 " + a + " " + b, (CmpDefaultU64 (a, b)), want);
				Check (tier, "EqDefaultU64 " + a + " " + b, EqDefaultU64 (a, b), (ulong) a == (ulong) b);
			}
		}

		Check (tier, "EqObjectS32 same", EqObjectS32 (S32.Neg, S32.Neg), true);
		Check (tier, "EqObjectS32 other value", EqObjectS32 (S32.Neg, S32.Max), false);
		Check (tier, "EqObjectS32 null", EqObjectS32 (S32.Zero, null), false);
		Check (tier, "EqObjectS32 underlying int", EqObjectS32 (S32.Neg, -1), false);
		Check (tier, "EqObjectS32 other enum", EqObjectS32 (S32.Zero, U32.Zero), false);
		Check (tier, "EqObjectS32 string", EqObjectS32 (S32.Zero, "Zero"), false);
		Check (tier, "EqObjectU8 same", EqObjectU8 (U8.High, U8.High), true);
		Check (tier, "EqObjectU8 underlying byte", EqObjectU8 (U8.High, (byte) 0x80), false);
		Check (tier, "EqObjectU8 wider enum, same bytes", EqObjectU8 (U8.Max, U16.Max), false);

		Check (tier, "TypeUnderlyingU16", TypeUnderlyingU16 (), typeof (ushort));
		Check (tier, "TypeCodeS64", TypeCodeS64 (S64.Neg), TypeCode.Int64);
		Check (tier, "TypeIsEnumU8", TypeIsEnumU8 (), true);
		Check (tier, "TypeIsEnumInt", TypeIsEnumInt (), false);

		object boxed = BoxS8 (-1);
		Check (tier, "BoxS8 (-1) class", boxed.GetType (), typeof (S8));
		Check (tier, "BoxS8 (-1)", (S8) boxed, S8.Neg);
		Check (tier, "BoxS8 (0x17f) truncates", (S8) BoxS8 (0x17f), S8.Max);
		Check (tier, "BoxU8 (300) truncates", (U8) BoxU8 (300), (U8) 44);
		Check (tier, "BoxU8 (0x80)", (U8) BoxU8 (0x80), U8.High);
		Check (tier, "BoxU64 (max)", (U64) BoxU64 (ulong.MaxValue), U64.Max);

		bool threw = false;
		try {
			BoxNotEnum (1);
		} catch (ArgumentException) {
			threw = true;
		}
		Check (tier, "BoxNotEnum throws", threw, true);

		for (int a = sbyte.MinValue; a <= sbyte.MaxValue; a++)
			Check (tier, "DefinedS8 " + a, DefinedS8 ((S8) a), a == sbyte.MinValue || a == -1 || a == 0 || a == sbyte.MaxValue);
		foreach (ulong a in new ulong [] { 0, 1, 0x7fffffffffffffff, 0x8000000000000000, 0xfffffffffffffffe, ulong.MaxValue })
			Check (tier, "DefinedU64 " + a, DefinedU64 ((U64) a), a != 1 && a != 0xfffffffffffffffe);
		foreach (int a in new int [] { int.MinValue, -1, 0, 3, 4, int.MaxValue })
			Check (tier, "DefinedRun " + a, DefinedRun ((Run) a), a >= 0 && a <= 3);
		for (int a = sbyte.MinValue; a <= sbyte.MaxValue; a++)
			Check (tier, "DefinedRunThroughZero " + a, DefinedRunThroughZero ((RunThroughZero) a), a >= -2 && a <= 1);
		foreach (ulong a in new ulong [] { 0, 1, 2, 0x8000000000000000, ulong.MaxValue - 2, ulong.MaxValue - 1, ulong.MaxValue })
			Check (tier, "DefinedRunThroughMax " + a, DefinedRunThroughMax ((RunThroughMax) a), a == 0 || a >= ulong.MaxValue - 1);
		for (int a = -1; a <= 66; a++) {
			Check (tier, "DefinedRuns32 " + a, DefinedRuns32 ((Runs32) a), a >= 0 && a <= 63 && (a % 2 == 0 || a == 63));
			Check (tier, "DefinedRuns33 " + a, DefinedRuns33 ((Runs33) a), a >= 0 && a <= 64 && a % 2 == 0);
		}
		Check (tier, "DefinedU8Byte (0x80)", DefinedU8Byte (0x80), true);
		Check (tier, "DefinedU8Byte (0x81)", DefinedU8Byte (0x81), false);
		Check (tier, "DefinedEmpty (0)", DefinedEmpty (0), false);
		Check (tier, "DefinedAliased (1)", DefinedAliased (Aliased.Uno), true);
		Check (tier, "DefinedAliased (2)", DefinedAliased ((Aliased) 2), false);
		Check (tier, "DefinedAliased (3)", DefinedAliased (Aliased.Three), true);
		Check (tier, "DefinedName (Max)", DefinedName ("Max"), true);
		Check (tier, "DefinedName (Neither)", DefinedName ("Neither"), false);

		threw = false;
		try {
			DefinedS16Int (-1);
		} catch (ArgumentException) {
			threw = true;
		}
		Check (tier, "DefinedS16Int throws", threw, true);

		threw = false;
		try {
			DefinedS32OtherEnum (S16.Neg);
		} catch (ArgumentException) {
			threw = true;
		}
		Check (tier, "DefinedS32OtherEnum throws", threw, true);

		Check (tier, "ValueS32 (Min)", ValueS32 (S32.Min), (long) int.MinValue);
		Check (tier, "ValueU32 (Max)", ValueU32 (U32.Max), "4294967295");
		Check (tier, "ValueIConvertibleS16 (Neg)", ValueIConvertibleS16 (S16.Neg), -1);
		Check (tier, "ValueNameU8 (High)", ValueNameU8 (U8.High), "High");
		Check (tier, "ValueNameU8 (5)", ValueNameU8 ((U8) 5), "5");
	}

	static void CheckComparerObject ()
	{
		var sorted = new List<U32> { U32.Max, U32.Zero, U32.High, U32.Mid };
		sorted.Sort ();
		Check ("corlib", "List<U32>.Sort ()", string.Join (",", sorted), "Zero,Mid,High,Max");

		var stream = new MemoryStream ();
		var formatter = new BinaryFormatter ();
		formatter.Serialize (stream, Comparer<S8>.Default);
		string written = System.Text.Encoding.UTF8.GetString (stream.ToArray ());
		Check ("corlib", "Comparer<S8> serialized as ObjectComparer",
		       written.Contains ("System.Collections.Generic.ObjectComparer`1"), true);

		stream.Position = 0;
		var read = (Comparer<S8>) formatter.Deserialize (stream);
		Check ("corlib", "deserialized Comparer<S8>", read.Compare (S8.Min, S8.Max), -1);
	}

	static bool Promote (MethodInfo target, int tier)
	{
		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: {0} () would not compile at tier {1}", target.Name, tier);
		return false;
	}

	static bool PromoteAll (int tier)
	{
		bool ok = true;

		foreach (MethodInfo m in typeof (Program).GetMethods (BindingFlags.Static | BindingFlags.NonPublic)) {
			if (m.Name.StartsWith ("Hash") || m.Name.StartsWith ("Cmp") || m.Name.StartsWith ("Eq")
			    || m.Name.StartsWith ("Type") || m.Name.StartsWith ("Box") || m.Name.StartsWith ("Value")
			    || m.Name.StartsWith ("Defined")) {
				MethodInfo target = m.IsGenericMethodDefinition ? null : m;

				if (target != null)
					ok &= Promote (target, tier);
			}
		}

		ok &= Promote (typeof (Program).GetMethod ("CmpConstrained", BindingFlags.Static | BindingFlags.NonPublic)
		               .MakeGenericMethod (typeof (U32)), tier);
		ok &= Promote (typeof (Program).GetMethod ("CmpConstrained", BindingFlags.Static | BindingFlags.NonPublic)
		               .MakeGenericMethod (typeof (S64)), tier);
		return ok;
	}

	public static int Main ()
	{
		CheckComparerObject ();
		RunAll ("tier 0");

		if (!PromoteAll (tier1))
			return 1;
		RunAll ("tier 1");

		if (!PromoteAll (tier2))
			return 1;
		RunAll ("tier 2");

		if (failures != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}
