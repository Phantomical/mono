using System;
using System.Reflection;
using System.Threading;

// -mono-tier1-threshold=0 is the setting a test reaches for when it wants a
// method pinned at tier 0, and this file is what says it holds for a classic
// body. arm_tier0_counter () (mono/mini/domain-method.cpp) writes -1 for a
// threshold of zero, and mono_tier0_count () reads any count at or below zero
// as one that has just run out, so a body that reaches that icall at all asks
// for tier 1 on its first call.
//
// Select-all rather than a filter: the counter this exercises is the one a
// classic body carries, and every method here has to have one. The loop and
// the calls both reach it - one at the entry and one at the back edge.
namespace Mono.Tiering {
	static class MonoTier {
		[System.Runtime.CompilerServices.MethodImpl (System.Runtime.CompilerServices.MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
		[System.Runtime.CompilerServices.MethodImpl (System.Runtime.CompilerServices.MethodImplOptions.InternalCall)]
		public static extern int GetTier (IntPtr method);
	}
}

public class Tier0ClassicThresholdZeroTest
{
	const int tier0 = 2;

	static long ThresholdZeroSpin (long n)
	{
		long acc = 0;

		for (long i = 0; i < n; i++)
			acc += i;

		return acc;
	}

	public static int Main ()
	{
		MethodInfo method = typeof (Tier0ClassicThresholdZeroTest).GetMethod (
			"ThresholdZeroSpin", BindingFlags.Static | BindingFlags.NonPublic);
		IntPtr handle = method.MethodHandle.Value;

		const long want = 999L * 1000L / 2L;

		for (int i = 0; i < 1000; i++) {
			long got = ThresholdZeroSpin (1000);

			if (got != want) {
				Console.WriteLine ("FAIL: wrong sum, got {0} want {1}", got, want);
				return 1;
			}
		}

		// A promotion the calls above asked for is queued, so it lands on a
		// compile worker rather than on this thread.
		Thread.Sleep (500);

		int tier = Mono.Tiering.MonoTier.GetTier (handle);

		if (tier != tier0) {
			Console.WriteLine ("FAIL: ThresholdZeroSpin () left tier 0 for tier {0}", tier);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
