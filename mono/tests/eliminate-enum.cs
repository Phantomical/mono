using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * GetHashCode (), Equals () and CompareTo () on an enum, and the comparers
 * Comparer<T>.Default and EqualityComparer<T>.Default hand out for one, over
 * every underlying type C# can declare. Each probe runs at tier 0, then at
 * tier 1 and tier 2, and every tier has to give the answer the underlying
 * primitive gives. Each underlying type's extremes are there because a load
 * or a compare of the wrong width or signedness gets exactly those wrong.
 */

enum S8 : sbyte { Min = sbyte.MinValue, Neg = -1, Zero = 0, Max = sbyte.MaxValue }
enum U8 : byte { Zero = 0, Mid = 0x7f, High = 0x80, Max = byte.MaxValue }
enum S16 : short { Min = short.MinValue, Neg = -1, Zero = 0, Max = short.MaxValue }
enum U16 : ushort { Zero = 0, Mid = 0x7fff, High = 0x8000, Max = ushort.MaxValue }
enum S32 { Min = int.MinValue, Neg = -1, Zero = 0, Max = int.MaxValue }
enum U32 : uint { Zero = 0, Mid = 0x7fffffff, High = 0x80000000, Max = uint.MaxValue }
enum S64 : long { Min = long.MinValue, Neg = -1, Zero = 0, Max = long.MaxValue }
enum U64 : ulong { Zero = 0, Mid = 0x7fffffffffffffff, High = 0x8000000000000000, Max = ulong.MaxValue }

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
			if (m.Name.StartsWith ("Hash") || m.Name.StartsWith ("Cmp") || m.Name.StartsWith ("Eq")) {
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
