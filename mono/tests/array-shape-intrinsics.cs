using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * The System.Array shape accessors the compiled tiers answer from the object
 * instead of from an icall: Rank, Length, GetLength (0) and GetLowerBound (0).
 *
 * Every receiver here is typed as Array. C# turns `a.Length` on an int[] into
 * ldlen, so a test that holds its arrays in their own type never reaches the
 * accessors this covers.
 *
 * The three shapes are what tell a right emitter from a wrong one. A szarray
 * carries no bounds vector, so its length comes from MonoArray.max_length and
 * its lower bound is zero. A rectangular array carries one. `bounded` is a
 * rank-1 array that carries one as well, and it is the case an emitter that
 * reads max_length for every rank-1 array gets wrong.
 *
 * Sample () is compared entry by entry across the tiers, because the
 * interpreter answers each accessor with the icall and the compiled tiers with
 * loads. Pinned () holds the answers the shapes settle, which agreement between
 * the tiers cannot supply.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Program {
	static int fails;

	static Array flat = new int[7];
	static Array square = new int[2, 3];
	static Array bounded = Array.CreateInstance (typeof (int), new int[] { 4 },
	                                             new int[] { 5 });
	static Array empty = new int[0];

	/// A dimension out of a field, so that the site keeps its call.
	static int dimension;

	static Array nothing;

	static void Fail (string what)
	{
		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	static void Same (string what, int got, int want)
	{
		if (got == want)
			return;

		Fail (String.Format ("{0} gave {1}, wanted {2}", what, got, want));
	}

	static int RankOf (Array a) { return a.Rank; }
	static int LengthOf (Array a) { return a.Length; }
	static int FirstLengthOf (Array a) { return a.GetLength (0); }
	static int FirstLowerBoundOf (Array a) { return a.GetLowerBound (0); }

	static int[] Sample ()
	{
		return new int[] {
			RankOf (flat), LengthOf (flat),
			FirstLengthOf (flat), FirstLowerBoundOf (flat),

			RankOf (square), LengthOf (square),
			FirstLengthOf (square), FirstLowerBoundOf (square),

			RankOf (bounded), LengthOf (bounded),
			FirstLengthOf (bounded), FirstLowerBoundOf (bounded),

			RankOf (empty), LengthOf (empty),
			FirstLengthOf (empty), FirstLowerBoundOf (empty),

			// Dimension one is a constant the emitters decline, and a
			// dimension out of a field is one they cannot see. Both keep
			// the call, and both have to agree with the rest.
			square.GetLength (1), square.GetLowerBound (1),
			flat.GetLength (dimension), flat.GetLowerBound (dimension),
		};
	}

	static readonly int[] pinned = new int[] {
		1, 7, 7, 0,
		2, 6, 2, 0,
		1, 4, 4, 5,
		1, 0, 0, 0,
		3, 0, 7, 0,
	};

	static void Pinned (string tier)
	{
		int[] got = Sample ();

		for (int i = 0; i < pinned.Length; i++)
			Same (String.Format ("{0} entry {1}", tier, i), got[i], pinned[i]);
	}

	static void CompareWith (string tier, int[] first, int[] now)
	{
		for (int i = 0; i < first.Length; i++)
			Same (String.Format ("{0} entry {1}", tier, i), now[i], first[i]);
	}

	/// A null receiver raises NullReferenceException at every tier. The icall
	/// raised one from a fault on the field the emitters now load.
	static void Null (string tier)
	{
		Raises (tier + " Rank", delegate { RankOf (nothing); });
		Raises (tier + " Length", delegate { LengthOf (nothing); });
		Raises (tier + " GetLength (0)", delegate { FirstLengthOf (nothing); });
		Raises (tier + " GetLowerBound (0)", delegate { FirstLowerBoundOf (nothing); });
	}

	static void Raises (string what, Action body)
	{
		try {
			body ();
		} catch (NullReferenceException) {
			return;
		}

		Fail (what + " on a null array did not raise NullReferenceException");
	}

	/// Array.Copy reads the rank, the lower bound and the length of both
	/// arrays before it moves one element, which is where these accessors
	/// cost. This pair declines FastCopy, so it takes that path.
	static void Copy (string tier)
	{
		object[] from = new object[] { 3, 1, 4, 1, 5 };
		int[] into = new int[5];

		Array.Copy (from, into, from.Length);

		for (int i = 0; i < into.Length; i++)
			Same (String.Format ("{0} copy {1}", tier, i), into[i], (int) from[i]);
	}

	static bool Promote (MethodInfo method, int tier, string what)
	{
		if (Mono.Tiering.MonoTier.PromoteNow (method.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: {0} would not compile at tier {1}", what, tier);
		++fails;
		return false;
	}

	static readonly string[] compiled = new string[] {
		"Sample", "RankOf", "LengthOf", "FirstLengthOf", "FirstLowerBoundOf", "Copy"
	};

	static bool PromoteAll (int tier)
	{
		foreach (string name in compiled) {
			MethodInfo one = typeof (Program).GetMethod (name,
				BindingFlags.Static | BindingFlags.NonPublic);

			if (!Promote (one, tier, name + " ()"))
				return false;
		}

		return true;
	}

	public static int Main ()
	{
		int[] first = Sample ();

		Pinned ("tier 0");
		Null ("tier 0");
		Copy ("tier 0");

		/*
		 * Asked for rather than waited for. An interpreted caller reaches an
		 * interpreted callee without the runtime being asked for it, so a loop
		 * alone leaves both methods where they started.
		 */
		if (!PromoteAll (2))
			return 1;

		CompareWith ("tier 1", first, Sample ());
		Pinned ("tier 1");
		Null ("tier 1");
		Copy ("tier 1");

		// Counts for the tier-2 compile to lay the bodies out against.
		for (int i = 0; i < 200; i++)
			Sample ();

		if (!PromoteAll (3))
			return 1;

		CompareWith ("tier 2", first, Sample ());
		Pinned ("tier 2");
		Null ("tier 2");
		Copy ("tier 2");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
