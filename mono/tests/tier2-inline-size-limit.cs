using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * -mono-inline-size-limit bounds how large, in IR instructions, Root may grow
 * through tier-2 folding, checked after every accepted fold and again after
 * each round's own simplification. Cheap1, Cheap2 and Costly together still
 * fit under the tight limit this suite pins; TooMuch, the most expensive of
 * the four, does not, and is left calling its published entry.
 *
 * Costly is what tells the ordering apart from a flat per-site check: it
 * costs more than Cheap1 or Cheap2 and its call site is written after both of
 * theirs, so folding it at all needs the round to have applied the cheaper
 * two first and still have room left, rather than applying candidates in the
 * order Root happens to call them in.
 *
 * Each candidate reads from data at an index the compile cannot resolve to a
 * constant, so the reads survive simplification instead of collapsing into a
 * closed form the way a chain of pure arithmetic on one local would. Each
 * carries a branch, which keeps the shape-test pre-pass out of it and leaves
 * the tier-2 cost model as the only inliner that can fold it.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Helpers {
	public static bool folded_cheap1, folded_cheap2, folded_costly, folded_toomuch;

	/* Whether the caller reading this frame is Root () itself, rather than a
	 * real call arriving through helper's own thunk. */
	static bool RunsInside (string helper, string root)
	{
		StackTrace st = new StackTrace (false);
		int in_helper = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Helpers" && m.Name == helper)
				in_helper = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == root)
				in_root = f.GetNativeOffset ();
		}

		return in_helper >= 0 && in_helper == in_root;
	}

	public static int Cheap1 (int[] data, int x)
	{
		if (x < 0)
			return -1;

		int s = data[0] + x;

		folded_cheap1 |= RunsInside ("Cheap1", "Root");
		return s;
	}

	public static int Cheap2 (int[] data, int x)
	{
		if (x < 0)
			return -1;

		int s = data[1] + x * 2;

		folded_cheap2 |= RunsInside ("Cheap2", "Root");
		return s;
	}

	public static int Costly (int[] data, int x)
	{
		if (x < 0)
			return -1;

		int s = x;
		s += data[0 % data.Length] ^ (0 * x);
		s += data[1 % data.Length] ^ (1 * x);
		s += data[2 % data.Length] ^ (2 * x);
		s += data[3 % data.Length] ^ (3 * x);
		s += data[4 % data.Length] ^ (4 * x);
		s += data[5 % data.Length] ^ (5 * x);
		s += data[6 % data.Length] ^ (6 * x);
		s += data[7 % data.Length] ^ (7 * x);
		s += data[8 % data.Length] ^ (8 * x);
		s += data[9 % data.Length] ^ (9 * x);
		s += data[10 % data.Length] ^ (10 * x);
		s += data[11 % data.Length] ^ (11 * x);
		s += data[12 % data.Length] ^ (12 * x);
		s += data[13 % data.Length] ^ (13 * x);
		s += data[14 % data.Length] ^ (14 * x);

		folded_costly |= RunsInside ("Costly", "Root");
		return s;
	}

	public static int TooMuch (int[] data, int x)
	{
		if (x < 0)
			return -1;

		int s = x;
		s += data[0 % data.Length] ^ (0 * x);
		s += data[1 % data.Length] ^ (1 * x);
		s += data[2 % data.Length] ^ (2 * x);
		s += data[3 % data.Length] ^ (3 * x);
		s += data[4 % data.Length] ^ (4 * x);
		s += data[5 % data.Length] ^ (5 * x);
		s += data[6 % data.Length] ^ (6 * x);
		s += data[7 % data.Length] ^ (7 * x);
		s += data[8 % data.Length] ^ (8 * x);
		s += data[9 % data.Length] ^ (9 * x);
		s += data[10 % data.Length] ^ (10 * x);
		s += data[11 % data.Length] ^ (11 * x);
		s += data[12 % data.Length] ^ (12 * x);
		s += data[13 % data.Length] ^ (13 * x);
		s += data[14 % data.Length] ^ (14 * x);
		s += data[15 % data.Length] ^ (15 * x);
		s += data[16 % data.Length] ^ (16 * x);
		s += data[17 % data.Length] ^ (17 * x);
		s += data[18 % data.Length] ^ (18 * x);
		s += data[19 % data.Length] ^ (19 * x);
		s += data[20 % data.Length] ^ (20 * x);
		s += data[21 % data.Length] ^ (21 * x);
		s += data[22 % data.Length] ^ (22 * x);
		s += data[23 % data.Length] ^ (23 * x);
		s += data[24 % data.Length] ^ (24 * x);
		s += data[25 % data.Length] ^ (25 * x);
		s += data[26 % data.Length] ^ (26 * x);
		s += data[27 % data.Length] ^ (27 * x);
		s += data[28 % data.Length] ^ (28 * x);
		s += data[29 % data.Length] ^ (29 * x);
		s += data[30 % data.Length] ^ (30 * x);
		s += data[31 % data.Length] ^ (31 * x);
		s += data[32 % data.Length] ^ (32 * x);
		s += data[33 % data.Length] ^ (33 * x);
		s += data[34 % data.Length] ^ (34 * x);
		s += data[35 % data.Length] ^ (35 * x);
		s += data[36 % data.Length] ^ (36 * x);
		s += data[37 % data.Length] ^ (37 * x);
		s += data[38 % data.Length] ^ (38 * x);
		s += data[39 % data.Length] ^ (39 * x);
		s += data[40 % data.Length] ^ (40 * x);
		s += data[41 % data.Length] ^ (41 * x);
		s += data[42 % data.Length] ^ (42 * x);
		s += data[43 % data.Length] ^ (43 * x);
		s += data[44 % data.Length] ^ (44 * x);
		s += data[45 % data.Length] ^ (45 * x);
		s += data[46 % data.Length] ^ (46 * x);
		s += data[47 % data.Length] ^ (47 * x);
		s += data[48 % data.Length] ^ (48 * x);
		s += data[49 % data.Length] ^ (49 * x);
		s += data[50 % data.Length] ^ (50 * x);
		s += data[51 % data.Length] ^ (51 * x);
		s += data[52 % data.Length] ^ (52 * x);
		s += data[53 % data.Length] ^ (53 * x);
		s += data[54 % data.Length] ^ (54 * x);
		s += data[55 % data.Length] ^ (55 * x);
		s += data[56 % data.Length] ^ (56 * x);
		s += data[57 % data.Length] ^ (57 * x);
		s += data[58 % data.Length] ^ (58 * x);
		s += data[59 % data.Length] ^ (59 * x);

		folded_toomuch |= RunsInside ("TooMuch", "Root");
		return s;
	}
}

static class Program {
	static void Root (int[] data, int n)
	{
		Helpers.Cheap1 (data, n + 0);
		Helpers.Cheap2 (data, n + 1);
		Helpers.Costly (data, n + 2);
		Helpers.TooMuch (data, n + 3);
	}

	static int fails;

	static void Check (bool ok, string what)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	/* MonoTier::tier1 and ::tier2, as PromoteNow takes them. */
	const int tier1 = 2;
	const int tier2 = 3;

	public static int Main ()
	{
		int[] data = new int[64];

		for (int i = 0; i < data.Length; i++)
			data[i] = i * 7;

		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, tier1)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		Root (data, 1);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, tier2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		Root (data, 1);

		Check (Helpers.folded_cheap1, "the cheapest candidate folds");
		Check (Helpers.folded_cheap2, "the second-cheapest candidate folds too");
		Check (Helpers.folded_costly,
		       "a pricier candidate written after both still folds, because " +
		       "the round applies cheapest first rather than call order");
		Check (!Helpers.folded_toomuch,
		       "the priciest candidate is left calling its published entry " +
		       "once the size limit the first three already spent leaves it " +
		       "no room");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
