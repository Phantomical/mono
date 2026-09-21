using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

/* Exercise the fence and pause intrinsics through tier 0 and tier 2. The
 * payload field is plain; visibility comes from the explicit barriers. */

class Program {
	const int tier2 = 4;

	static int payload;
	static volatile bool ready;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Publish (int value)
	{
		payload = value;
		Thread.MemoryBarrier ();
		ready = true;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Consume ()
	{
		while (!ready)
			Thread.SpinWait (1);

		Thread.MemoryBarrier ();
		return payload;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Handoff (int value)
	{
		payload = 0;
		ready = false;

		int seen = 0;
		Thread consumer = new Thread (() => seen = Consume ());

		consumer.Start ();
		Publish (value);
		consumer.Join ();

		return seen;
	}

	static bool Promote (MethodInfo target)
	{
		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier2))
			return true;

		Console.WriteLine ("FAIL: {0} () would not compile at tier 2", target.Name);
		return false;
	}

	static bool Promote (string name)
	{
		return Promote (typeof (Program).GetMethod (
			name, BindingFlags.Static | BindingFlags.NonPublic));
	}

	public static int Main ()
	{
		for (int i = 0; i < 20; i++) {
			int got = Handoff (i + 1);

			if (got != i + 1) {
				Console.WriteLine ("FAIL: tier 0 handoff {0} answered {1}", i, got);
				return 1;
			}
		}

		if (!Promote ("Publish") || !Promote ("Consume") || !Promote ("Handoff"))
			return 1;
		if (!Promote (typeof (Thread).GetMethod ("SpinWait", new[] { typeof (int) })))
			return 1;

		for (int i = 0; i < 20; i++) {
			int got = Handoff (i + 1);

			if (got != i + 1) {
				Console.WriteLine ("FAIL: tier 2 handoff {0} answered {1}", i, got);
				return 1;
			}
		}

		// Also cover Interlocked.MemoryBarrier (), which forwards to Thread.MemoryBarrier ().
		Interlocked.MemoryBarrier ();

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
