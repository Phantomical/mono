using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

/*
 * A thread static reached off the thread instead of through the runtime.
 *
 * thread_static_slot () takes the block index and the offset a thread static has
 * in the domain, and thread_static_address () emits a TLS read, two loads and an
 * add. The two loads carry !invariant.load, so LLVM shares one address between
 * the sites a body holds and lifts it out of a loop.
 *
 * Every case below runs first interpreted and then at tier 2, and compares the
 * two. --llvm-opt=-mono-thread-static-fast-path=0 puts the compiled arm back
 * on the icall and every case has to answer the same.
 *
 * The second thread Main () starts is the arm the sharing can break: an address
 * that outlived the frame it was read in would give that thread the first
 * thread's storage. Context () is the arm that must stay on the icall, because a
 * context static lives on the MonoAppContext.
 */

struct Pair {
	public int Low;
	public int High;
}

class Program {
	const int tier2 = 2;

	[ThreadStatic]
	static int counter;

	[ThreadStatic]
	static object slot;

	[ThreadStatic]
	static Pair pair;

	[ContextStatic]
	static int context_counter;

	static int ordinary;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Opaque (int value)
	{
		return value;
	}

	/// Sums the field over a loop that also writes it, which is the shape one
	/// shared address serves. Answers n * (n + 1) / 2 from a zeroed slot.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Accumulate (int n)
	{
		counter = 0;

		for (int i = 1; i <= n; i++)
			counter = counter + i;

		return counter;
	}

	/// Reads the field either side of a call, so a stale address shows up as the
	/// two disagreeing.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int AcrossACall ()
	{
		counter = 7;

		int before = counter;
		int opaque = Opaque (counter);

		counter = counter + 1;

		int after = counter;

		if (before != 7 || opaque != 7 || after != 8)
			return -1;

		return after;
	}

	/// A reference thread static, which the collector has to keep marking.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Reference ()
	{
		slot = new object ();

		object first = slot;

		GC.Collect ();

		if (!ReferenceEquals (first, slot))
			return -1;

		return first.GetHashCode () == slot.GetHashCode () ? 1 : -1;
	}

	/// A value-type thread static, which is reached through its address rather
	/// than loaded whole.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Struct (int seed)
	{
		pair.Low = seed;
		pair.High = seed * 2;

		return pair.Low + pair.High;
	}

	/// Takes the field's address, which is the ldsflda path rather than ldsfld.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ByAddress ()
	{
		counter = 0;

		Interlocked.Increment (ref counter);
		Interlocked.Add (ref counter, 41);

		return counter;
	}

	/// A context static keeps the icall, and still has to answer.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Context ()
	{
		context_counter = 0;

		for (int i = 1; i <= 10; i++)
			context_counter = context_counter + i;

		return context_counter;
	}

	/// An ordinary static beside the thread ones, so a wrong block index that
	/// happened to land on live storage is visible as this changing.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Ordinary ()
	{
		ordinary = 1234;
		counter = 99;

		return ordinary;
	}

	static bool Check (string name, int got, int want)
	{
		if (got == want)
			return true;

		Console.WriteLine ("FAIL: {0} answered {1}, wanted {2}", name, got, want);
		return false;
	}

	static bool RunAll (string tier)
	{
		bool ok = true;

		ok &= Check (tier + " Accumulate", Accumulate (100), 5050);
		ok &= Check (tier + " AcrossACall", AcrossACall (), 8);
		ok &= Check (tier + " Reference", Reference (), 1);
		ok &= Check (tier + " Struct", Struct (11), 33);
		ok &= Check (tier + " ByAddress", ByAddress (), 42);
		ok &= Check (tier + " Context", Context (), 55);
		ok &= Check (tier + " Ordinary", Ordinary (), 1234);

		return ok;
	}

	static bool Promote (string name)
	{
		MethodInfo target = typeof (Program).GetMethod (
			name, BindingFlags.Static | BindingFlags.NonPublic);

		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier2))
			return true;

		Console.WriteLine ("FAIL: {0} () would not compile at tier 2", name);
		return false;
	}

	public static int Main ()
	{
		if (!RunAll ("tier 0"))
			return 1;

		string[] bodies = {
			"Accumulate", "AcrossACall", "Reference", "Struct",
			"ByAddress", "Context", "Ordinary", "Opaque",
		};

		foreach (string body in bodies)
			if (!Promote (body))
				return 1;

		if (!RunAll ("tier 2"))
			return 1;

		// This thread left counter at 99 and pair set. A second thread has to
		// find its own storage zeroed, whatever address the compiled body shares.
		int seen = -1;
		int seen_pair = -1;
		bool other_ok = false;

		Thread second = new Thread (() => {
			seen = counter;
			seen_pair = pair.Low + pair.High;
			other_ok = RunAll ("second thread");
		});

		second.Start ();
		second.Join ();

		if (!other_ok)
			return 1;

		if (seen != 0 || seen_pair != 0) {
			Console.WriteLine ("FAIL: the second thread read counter {0} and pair {1}",
			                   seen, seen_pair);
			return 1;
		}

		// The first thread's storage survived the second thread running the same
		// compiled bodies.
		if (counter != 99) {
			Console.WriteLine ("FAIL: counter is {0} after the second thread ran", counter);
			return 1;
		}

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
