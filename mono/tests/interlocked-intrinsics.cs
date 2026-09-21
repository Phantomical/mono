using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

/*
 * Exercise the scalar and object overloads at tier 0, tier 1, and tier 2.
 * The object round also checks that the atomic lowering emits the collector's
 * write barrier.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Payload {
	public int Value;
	public Payload (int value) { Value = value; }
}

class Holder {
	public object Field;
}

class Driver {
	/* MonoTier::tier1 and MonoTier::tier2, as PromoteNow takes them. */
	const int tier1 = 3;
	const int tier2 = 4;

	const int Count = 20000;

	static Holder[] holders;

	static bool Wrong (string what, int got, int want)
	{
		if (got == want)
			return false;

		Console.WriteLine ("FAIL: {0} reads {1}, wanted {2}", what, got, want);
		return true;
	}

	static int CheckIncrementDecrement ()
	{
		int i32 = 41;
		if (Interlocked.Increment (ref i32) != 42) return 10;
		if (i32 != 42) return 11;
		if (Interlocked.Decrement (ref i32) != 41) return 12;
		if (i32 != 41) return 13;

		long i64 = 41;
		if (Interlocked.Increment (ref i64) != 42) return 14;
		if (i64 != 42) return 15;
		if (Interlocked.Decrement (ref i64) != 41) return 16;
		if (i64 != 41) return 17;

		return 0;
	}

	static int CheckAdd ()
	{
		int i32 = 10;
		if (Interlocked.Add (ref i32, 5) != 15) return 20;
		if (i32 != 15) return 21;

		long i64 = 10;
		if (Interlocked.Add (ref i64, 5L) != 15L) return 22;
		if (i64 != 15L) return 23;

		return 0;
	}

	static int CheckExchange ()
	{
		int i32 = 1;
		if (Interlocked.Exchange (ref i32, 2) != 1) return 30;
		if (i32 != 2) return 31;

		long i64 = 1;
		if (Interlocked.Exchange (ref i64, 2L) != 1L) return 32;
		if (i64 != 2L) return 33;

		IntPtr p = new IntPtr (1);
		if (Interlocked.Exchange (ref p, new IntPtr (2)) != new IntPtr (1)) return 34;
		if (p != new IntPtr (2)) return 35;

		float f = 1.5f;
		if (Interlocked.Exchange (ref f, 2.5f) != 1.5f) return 36;
		if (f != 2.5f) return 37;

		double d = 1.5;
		if (Interlocked.Exchange (ref d, 2.5) != 1.5) return 38;
		if (d != 2.5) return 39;

		return 0;
	}

	static int CheckCompareExchange ()
	{
		int i32 = 1;
		if (Interlocked.CompareExchange (ref i32, 2, 1) != 1) return 40;
		if (i32 != 2) return 41;
		if (Interlocked.CompareExchange (ref i32, 3, 1) != 2) return 42;
		if (i32 != 2) return 43;

		long i64 = 1;
		if (Interlocked.CompareExchange (ref i64, 2L, 1L) != 1L) return 44;
		if (i64 != 2L) return 45;
		if (Interlocked.CompareExchange (ref i64, 3L, 1L) != 2L) return 46;
		if (i64 != 2L) return 47;

		IntPtr p = new IntPtr (1);
		if (Interlocked.CompareExchange (ref p, new IntPtr (2), new IntPtr (1)) != new IntPtr (1))
			return 48;
		if (p != new IntPtr (2)) return 49;

		float f = 1.5f;
		if (Interlocked.CompareExchange (ref f, 2.5f, 1.5f) != 1.5f) return 50;
		if (f != 2.5f) return 51;

		double d = 1.5;
		if (Interlocked.CompareExchange (ref d, 2.5, 1.5) != 1.5) return 52;
		if (d != 2.5) return 53;

		return 0;
	}

	/// CompareExchange (ref int, int, int, ref bool) is internal, and no
	/// caller in mcs/class/corlib names it. Reflection is the only way to
	/// reach it from outside corlib, through an ordinary call
	/// mono_runtime_invoke () builds underneath.
	static int CheckCompareExchangeSuccess ()
	{
		MethodInfo mi = typeof (Interlocked).GetMethod ("CompareExchange",
			BindingFlags.NonPublic | BindingFlags.Static, null,
			new Type[] { typeof (int).MakeByRefType (), typeof (int), typeof (int),
			             typeof (bool).MakeByRefType () },
			null);

		if (mi == null)
			return 60;

		object[] args = new object[] { 1, 2, 1, false };
		int old = (int) mi.Invoke (null, args);

		if (old != 1) return 61;
		if ((int) args[0] != 2) return 62;
		if (!(bool) args[3]) return 63;

		args = new object[] { 5, 2, 1, false };
		old = (int) mi.Invoke (null, args);

		if (old != 5) return 64;
		if ((int) args[0] != 5) return 65;
		if ((bool) args[3]) return 66;

		return 0;
	}

	static int CheckExchangeObject ()
	{
		object a = new object ();
		object b = new object ();
		object location = a;

		object old = Interlocked.Exchange (ref location, b);

		if (!ReferenceEquals (old, a)) return 70;
		if (!ReferenceEquals (location, b)) return 71;

		return 0;
	}

	static int CheckCompareExchangeObject ()
	{
		object a = new object ();
		object b = new object ();
		object other = new object ();
		object location = a;

		object old = Interlocked.CompareExchange (ref location, b, a);

		if (!ReferenceEquals (old, a)) return 80;
		if (!ReferenceEquals (location, b)) return 81;

		old = Interlocked.CompareExchange (ref location, other, a);

		if (!ReferenceEquals (old, b)) return 82;
		if (!ReferenceEquals (location, b)) return 83;

		return 0;
	}

	static int CheckRead ()
	{
		long v = 0x1122334455667788L;

		if (Interlocked.Read (ref v) != v) return 90;

		return 0;
	}

	static int RunChecks ()
	{
		int rc;

		if ((rc = CheckIncrementDecrement ()) != 0) return rc;
		if ((rc = CheckAdd ()) != 0) return rc;
		if ((rc = CheckExchange ()) != 0) return rc;
		if ((rc = CheckCompareExchange ()) != 0) return rc;
		if ((rc = CheckCompareExchangeSuccess ()) != 0) return rc;
		if ((rc = CheckExchangeObject ()) != 0) return rc;
		if ((rc = CheckCompareExchangeObject ()) != 0) return rc;
		if ((rc = CheckRead ()) != 0) return rc;

		Interlocked.MemoryBarrier ();
		return 0;
	}

	// The barrier round below.

	static void ExchangeField (Holder holder, int value)
	{
		Interlocked.Exchange (ref holder.Field, new Payload (value));
	}

	static void CompareExchangeField (Holder holder, int value)
	{
		object previous = holder.Field;

		Interlocked.CompareExchange (ref holder.Field, new Payload (value), previous);
	}

	static void MakeHoldersOld ()
	{
		holders = new Holder[Count];

		for (int i = 0; i < Count; i++)
			holders[i] = new Holder ();

		GC.Collect ();
		GC.WaitForPendingFinalizers ();
		GC.Collect ();
	}

	/// Fills the nursery with garbage, so that a payload the collection freed
	/// has its bytes taken by something else. A stale pointer that still reads
	/// its old contents makes the check below pass on a card the store never
	/// marked.
	static void Churn ()
	{
		object[] sink = new object[64];

		for (int i = 0; i < Count * 8; i++)
			sink[i & 63] = new Payload (-1);
	}

	static bool BarrierRound (int round, string name)
	{
		for (int i = 0; i < Count; i++) {
			if ((i & 1) == 0)
				ExchangeField (holders[i], round + i);
			else
				CompareExchangeField (holders[i], round + i);
		}

		GC.Collect (0);
		Churn ();

		for (int i = 0; i < Count; i++) {
			int want = round + i;
			Payload got = holders[i].Field as Payload;

			if (got == null || Wrong ("holder field", got.Value, want)) {
				Console.WriteLine ("       at {0}", name);
				return false;
			}
		}

		return true;
	}

	static bool Promote (int tier, string name)
	{
		string[] methods = {
			"CheckIncrementDecrement", "CheckAdd", "CheckExchange", "CheckCompareExchange",
			"CheckExchangeObject", "CheckCompareExchangeObject", "CheckRead",
			"ExchangeField", "CompareExchangeField",
		};

		foreach (string method in methods) {
			MethodInfo info = typeof (Driver).GetMethod (
				method, BindingFlags.Static | BindingFlags.NonPublic);

			if (!Mono.Tiering.MonoTier.PromoteNow (info.MethodHandle.Value, tier)) {
				Console.WriteLine ("FAIL: {0} () would not compile at {1}", method, name);
				return false;
			}
		}

		return true;
	}

	static int Main ()
	{
		int rc;

		MakeHoldersOld ();

		// Tier 0 first: the classic compiler's own Interlocked lowering.
		if ((rc = RunChecks ()) != 0) return rc;
		if (!BarrierRound (1000000, "tier 0")) return 100;

		// Tier 1: this backend's FastISel path.
		if (!Promote (tier1, "tier 1")) return 101;
		if ((rc = RunChecks ()) != 0) return rc;
		if (!BarrierRound (2000000, "tier 1")) return 102;

		// Tier 2: the optimizing selector, past the trivial-inline pre-pass.
		if (!Promote (tier2, "tier 2")) return 103;
		if ((rc = RunChecks ()) != 0) return rc;
		if (!BarrierRound (3000000, "tier 2")) return 104;

		Console.WriteLine ("OK");
		return 0;
	}
}
