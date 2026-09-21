using System;
using System.Runtime.CompilerServices;
using System.Threading;

/*
 * Object.GetHashCode ()'s call to InternalGetHashCode () reads a hash
 * already cached in the object's lock word, instead of through the icall.
 * mono_object_hash_internal () (mono/metadata/monitor.c) is what publishes
 * that cache, in one of two forms this exercises:
 *
 *   - a thin hash, packed into the lock word itself, which a hash on a
 *     never-locked object always takes;
 *   - a hash inside the MonoThreadsSync an inflated lock word points to,
 *     which a hash cached before the object is ever locked has to survive,
 *     because locking an object that already carries a thin hash can only
 *     be represented by inflating it.
 *
 * GetHashCode () is reached by virtual dispatch here. It is GetHashCode ()
 * itself, not a caller of it, that has to be compiled through the backend
 * for the fast path to fire. Promotion is pinned to that one method, and
 * self-promotion is off, so Promote () alone decides when it happens.
 *
 * --llvm-opt=-mono-hash-fastpath=0 puts every call back on the icall, and
 * every case below has to give the same result.
 */

class Program {
	const int tier1 = 3;
	const int tier2 = 4;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Hash (object o)
	{
		return o.GetHashCode ();
	}

	/// A never-locked object, hashed twice: the second call reads the thin
	/// hash the first one published.
	static bool Thin ()
	{
		object o = new object ();
		int first = Hash (o);
		int second = Hash (o);

		if (first != second) {
			Console.WriteLine ("FAIL: Thin answered {0} then {1}", first, second);
			return false;
		}

		return true;
	}

	/// A hash cached before the object is ever locked. The lock then forces
	/// the inflated form to carry the value forward.
	static bool HashThenLock ()
	{
		object o = new object ();
		int before = Hash (o);

		lock (o) { }
		GC.Collect ();

		int after = Hash (o);

		if (before != after) {
			Console.WriteLine ("FAIL: HashThenLock answered {0} then {1}", before, after);
			return false;
		}

		return true;
	}

	/// The other order: an uncontended lock, released, then a first hash.
	/// Whichever form the icall picks, a second hash has to read the same
	/// form back.
	static bool LockThenHash ()
	{
		object o = new object ();

		lock (o) { }

		int first = Hash (o);
		int second = Hash (o);

		if (first != second) {
			Console.WriteLine ("FAIL: LockThenHash answered {0} then {1}", first, second);
			return false;
		}

		return true;
	}

	static bool RunAll (string tier)
	{
		bool ok = true;

		if (!Thin ()) {
			Console.WriteLine ("FAIL: {0} Thin", tier);
			ok = false;
		}
		if (!HashThenLock ()) {
			Console.WriteLine ("FAIL: {0} HashThenLock", tier);
			ok = false;
		}
		if (!LockThenHash ()) {
			Console.WriteLine ("FAIL: {0} LockThenHash", tier);
			ok = false;
		}

		return ok;
	}

	static bool Promote (int tier)
	{
		var target = typeof (object).GetMethod ("GetHashCode");

		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: object.GetHashCode () would not compile at tier {0}", tier);
		return false;
	}

	public static int Main ()
	{
		if (!RunAll ("tier 0"))
			return 1;

		if (!Promote (tier1) || !RunAll ("tier 1"))
			return 1;

		if (!Promote (tier2) || !RunAll ("tier 2"))
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
