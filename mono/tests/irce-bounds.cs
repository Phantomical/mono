using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/* Compare tier-2 IRCE results with tier 0, including failing bounds checks. */
static class Program {
	/* MonoTier::tier2, as PromoteNow takes it. */
	const int tier2 = 4;

	static int failures;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long Sum (int[] src, int n)
	{
		long s = 0;
		for (int i = 0; i < n; ++i)
			s += src[i];
		return s;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Copy (int[] src, int[] dst, int n)
	{
		for (int i = 0; i < n; ++i)
			dst[i] = src[i];
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void CopyFrom (int[] src, int[] dst, int start, int n)
	{
		for (int i = start; i < n; ++i)
			dst[i] = src[i];
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void CopyOffset (int[] src, int[] dst, int n, int k)
	{
		for (int i = 0; i < n; ++i)
			dst[i] = src[i + k];
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void CopyDown (int[] src, int[] dst, int n)
	{
		for (int i = n - 1; i >= 0; --i)
			dst[i] = src[i];
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long SumRows (int[][] rows, int n, int m)
	{
		long s = 0;
		for (int i = 0; i < n; ++i)
			for (int j = 0; j < m; ++j)
				s += rows[i][j];
		return s;
	}

	// The check's throw is caught inside the loop, so its failing arm stays in
	// the loop rather than leaving it.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CopyCatching (int[] src, int[] dst, int n)
	{
		int misses = 0;
		for (int i = 0; i < n; ++i) {
			try {
				dst[i] = src[i];
			} catch (IndexOutOfRangeException) {
				++misses;
			}
		}
		return misses;
	}

	static int[] Fill (int n)
	{
		int[] a = new int[n];
		for (int i = 0; i < n; ++i)
			a[i] = i * 7 + 1;
		return a;
	}

	static string Describe (int[] dst, string thrown, long value)
	{
		return string.Join (",", dst) + "|" + thrown + "|" + value;
	}

	/// Run one kernel and capture its observable result.
	static string Case (int which)
	{
		int[] src = Fill (16);
		int[] dst = new int[64];
		string thrown = "";
		long value = 0;

		try {
			switch (which) {
			case 0: Copy (src, dst, 16); break;
			case 1: Copy (src, dst, 40); break;
			case 2: CopyFrom (src, dst, 4, 16); break;
			case 3: CopyFrom (src, dst, -3, 16); break;
			case 4: CopyOffset (src, dst, 12, 4); break;
			case 5: CopyOffset (src, dst, 16, 4); break;
			case 6: CopyOffset (src, dst, 8, -2); break;
			case 7: CopyDown (src, dst, 16); break;
			case 8: CopyDown (src, dst, 30); break;
			case 9: value = SumRows (new[] { Fill (12), Fill (12), Fill (12) }, 3, 12); break;
			case 10: value = SumRows (new[] { Fill (12), Fill (5), Fill (12) }, 3, 12); break;
			case 11: value = SumRows (new[] { Fill (12), Fill (12) }, 3, 12); break;
			case 12: value = CopyCatching (src, dst, 40); break;
			case 13: Copy (src, new int[10], 16); break;
			case 14: value = Sum (src, 16); break;
			case 15: value = Sum (src, 40); break;
			}
		} catch (IndexOutOfRangeException) {
			thrown = "IndexOutOfRange";
		}

		return Describe (dst, thrown, value);
	}

	const int cases = 16;

	static readonly string[] kernels = {
		"Sum", "Copy", "CopyFrom", "CopyOffset", "CopyDown", "SumRows", "CopyCatching"
	};

	static bool Promote ()
	{
		foreach (string name in kernels) {
			MethodInfo target = typeof (Program).GetMethod (name,
				BindingFlags.Static | BindingFlags.NonPublic);

			if (!Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier2)) {
				Console.WriteLine ("FAIL: {0} () would not compile at tier 2", name);
				return false;
			}
		}

		return true;
	}

	public static int Main ()
	{
		string[] expect = new string[cases];
		for (int c = 0; c < cases; ++c)
			expect[c] = Case (c);

		if (!Promote ())
			return 1;

		for (int round = 0; round < 200; ++round) {
			for (int c = 0; c < cases; ++c) {
				string got = Case (c);
				if (got == expect[c])
					continue;

				Console.WriteLine ("FAIL case {0} round {1}:\n  got  {2}\n  want {3}", c, round, got, expect[c]);
				if (++failures > 8)
					return 1;
			}
		}

		if (failures > 0)
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
