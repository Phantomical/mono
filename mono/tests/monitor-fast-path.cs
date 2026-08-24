using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

/*
 * Monitor.Enter and Monitor.Exit, which the compiled tiers answer with a call
 * to a fast helper and the call the site named on the edge the helper did not
 * take.
 *
 * Every case runs at each of the three tiers, because only the compiled ones
 * have a helper in front and a difference between them is the fault this test
 * is for. Each helper answers an uncontended lock and refuses everything else,
 * so each case below picks which of the two edges runs:
 *
 * - Enter and Exit, and the same pair a second time around a lock this thread
 *   already holds, take the helper's edge.
 * - A lock another thread holds, a null object and a lockTaken that is already
 *   true take the call.
 * - For Exit, a nested lock, an inflated lock and a lock this thread does not
 *   own take the call as well.
 *
 * The lockTaken cases are the ones worth having on the Enter side. The helper
 * writes that flag itself, so an arm that takes the wrong edge leaves a lock
 * held with the flag false, and the finally block of a `lock` then never exits
 * it.
 *
 * ExitOtherThreadsLock is the one on the Exit side. A helper that answered it
 * would release a lock the calling thread does not hold, and the thread that
 * does hold it then finds the lock word already free.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Program {
	static int fails;

	static void Fail (string what)
	{
		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	static void Check (string what, bool ok)
	{
		if (!ok)
			Fail (what);
	}

	/// Takes and releases the lock through the two-argument overload, which is
	/// what C# compiles `lock` to.
	static bool EnterAndExit (object gate)
	{
		bool taken = false;

		try {
			Monitor.Enter (gate, ref taken);
			return taken && Monitor.IsEntered (gate);
		} finally {
			if (taken)
				Monitor.Exit (gate);
		}
	}

	/// The same, one level deeper, so the second Enter arrives at a lock this
	/// thread already owns and the helper answers it by counting the nest up.
	static bool EnterTwice (object gate)
	{
		bool outer = false;

		try {
			Monitor.Enter (gate, ref outer);

			if (!outer || !EnterAndExit (gate))
				return false;

			return Monitor.IsEntered (gate);
		} finally {
			if (outer)
				Monitor.Exit (gate);
		}
	}

	/// The one-argument overload, which is an internal call rather than IL.
	static bool EnterAndExitV1 (object gate)
	{
		Monitor.Enter (gate);

		try {
			return Monitor.IsEntered (gate);
		} finally {
			Monitor.Exit (gate);
		}
	}

	/// Whether Enter on a null object throws what the argument is owed.
	static bool EnterNull ()
	{
		bool taken = false;

		try {
			Monitor.Enter (null, ref taken);
		} catch (ArgumentNullException) {
			return !taken;
		}

		return false;
	}

	/// Whether Enter refuses a lockTaken that is already true. The helper reads
	/// the flag before it touches the object, so this case never takes the lock.
	static bool EnterAlreadyTaken (object gate)
	{
		bool taken = true;

		try {
			Monitor.Enter (gate, ref taken);
		} catch (ArgumentException) {
			return !Monitor.IsEntered (gate);
		} finally {
			if (Monitor.IsEntered (gate))
				Monitor.Exit (gate);
		}

		return false;
	}

	/// Whether Enter blocks and then takes a lock another thread holds, which is
	/// the case the helper refuses.
	static bool EnterContended (object gate)
	{
		var held = new ManualResetEventSlim ();
		var release = new ManualResetEventSlim ();
		var worker = new Thread (() => {
			lock (gate) {
				held.Set ();
				release.Wait ();
			}
		});

		worker.Start ();
		held.Wait ();

		// The worker still holds it, so this blocks until release lets go.
		var waiter = new Thread (() => {
			bool taken = false;

			try {
				Monitor.Enter (gate, ref taken);
			} finally {
				if (taken)
					Monitor.Exit (gate);
			}
		});

		waiter.Start ();
		release.Set ();
		worker.Join ();
		waiter.Join ();

		return !Monitor.IsEntered (gate);
	}

	/// Whether Exit on a null object throws what the argument is owed.
	static bool ExitNull ()
	{
		try {
			Monitor.Exit (null);
		} catch (ArgumentNullException) {
			return true;
		}

		return false;
	}

	/// Whether Exit refuses a lock another thread holds. The helper reads the
	/// owner out of the lock word, so an arm that answers this case frees a
	/// lock the worker below still believes it holds.
	static bool ExitOtherThreadsLock (object gate)
	{
		var held = new ManualResetEventSlim ();
		var release = new ManualResetEventSlim ();
		bool workerOk = false;
		var worker = new Thread (() => {
			lock (gate) {
				held.Set ();
				release.Wait ();
				workerOk = Monitor.IsEntered (gate);
			}
		});

		worker.Start ();
		held.Wait ();

		bool threw = false;

		try {
			Monitor.Exit (gate);
		} catch (SynchronizationLockException) {
			threw = true;
		}

		release.Set ();
		worker.Join ();

		return threw && workerOk && !Monitor.IsEntered (gate);
	}

	/// Whether a nest count comes down one Exit at a time. The helper answers
	/// the last of the two and refuses the first, which still holds the lock.
	static bool ExitNested (object gate)
	{
		Monitor.Enter (gate);
		Monitor.Enter (gate);

		Monitor.Exit (gate);

		if (!Monitor.IsEntered (gate))
			return false;

		Monitor.Exit (gate);

		return !Monitor.IsEntered (gate);
	}

	/// Whether an inflated lock exits, which is the case the helper refuses.
	///
	/// A hash of an object whose lock this thread holds moves the lock into a
	/// MonoThreadsSync structure. The object is this method's own, because an
	/// inflated lock stays inflated and would decide the edge every later case
	/// takes.
	static bool ExitInflated ()
	{
		object gate = new object ();

		Monitor.Enter (gate);
		gate.GetHashCode ();
		Monitor.Exit (gate);

		return !Monitor.IsEntered (gate);
	}

	static void RunAll (string tier)
	{
		object gate = new object ();

		Check (tier + ": EnterAndExit", EnterAndExit (gate));
		Check (tier + ": EnterTwice", EnterTwice (gate));
		Check (tier + ": EnterAndExitV1", EnterAndExitV1 (gate));
		Check (tier + ": EnterNull", EnterNull ());
		Check (tier + ": EnterAlreadyTaken", EnterAlreadyTaken (gate));
		Check (tier + ": EnterContended", EnterContended (gate));
		Check (tier + ": ExitNull", ExitNull ());
		Check (tier + ": ExitOtherThreadsLock", ExitOtherThreadsLock (gate));
		Check (tier + ": ExitNested", ExitNested (gate));
		Check (tier + ": ExitInflated", ExitInflated ());

		// The lock is nobody's once every case above has finished.
		Check (tier + ": the gate is free", !Monitor.IsEntered (gate));
	}

	static readonly string[] cases = {
		"EnterAndExit", "EnterTwice", "EnterAndExitV1",
		"EnterNull", "EnterAlreadyTaken", "EnterContended",
		"ExitNull", "ExitOtherThreadsLock", "ExitNested", "ExitInflated",
	};

	static bool Promote (int tier)
	{
		foreach (string name in cases) {
			MethodInfo method = typeof (Program).GetMethod (name,
				BindingFlags.Static | BindingFlags.NonPublic);

			if (Mono.Tiering.MonoTier.PromoteNow (method.MethodHandle.Value, tier))
				continue;

			Console.WriteLine ("FAIL: {0} would not compile at tier {1}", name, tier);
			++fails;
			return false;
		}

		return true;
	}

	public static int Main ()
	{
		RunAll ("tier 0");

		/*
		 * Asked for rather than waited for. An interpreted caller reaches an
		 * interpreted callee without the runtime being asked for it, so a loop
		 * alone leaves the methods where they started.
		 */
		if (!Promote (2))
			return 1;

		RunAll ("tier 1");

		/*
		 * Counts for the tier-2 compile to lay the bodies out against.
		 * EnterContended and ExitOtherThreadsLock are left out: each starts a
		 * thread per call, and the edge they exercise is the one the fast
		 * helper declines rather than the one the counts are about.
		 */
		object warm = new object ();

		for (int i = 0; i < 200; i++) {
			EnterAndExit (warm);
			EnterTwice (warm);
			EnterAndExitV1 (warm);
			EnterNull ();
			EnterAlreadyTaken (warm);
			ExitNull ();
			ExitNested (warm);
			ExitInflated ();
		}

		if (!Promote (3))
			return 1;

		RunAll ("tier 2");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
