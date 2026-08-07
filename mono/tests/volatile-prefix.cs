using System;
using System.Threading;

/*
 * The volatile. prefix (ECMA-335 III.2.6), through the only spelling C# has for
 * it: a volatile field. Two properties are pinned here.
 *
 * A volatile read cannot be cached, so a spin on one has to observe another
 * thread's store. The loop below is deliberately call-free and body-free -
 * exactly the shape an ordinary load gets hoisted out of - so if the prefix
 * stops meaning anything the spin never ends. A watchdog thread turns that
 * hang into a failure rather than leaving the harness to time it out.
 *
 * A volatile write has release semantics and a volatile read acquire semantics
 * (I.12.6.7), so ordinary writes placed before a volatile one are visible to
 * anybody who has seen it. Without the ordering nothing stops the two plain
 * stores in the writer from sinking past the volatile one, or the two plain
 * loads in the reader from being hoisted above it.
 */
class VolatilePrefixTest
{
	const int Rounds = 50000;

	static volatile bool flag;

	/* Published by the release on `ready`, read back after the acquire on it. */
	static int payload_a;
	static int payload_b;
	static volatile int ready;

	static void Watchdog ()
	{
		Thread.Sleep (30000);
		Console.WriteLine ("volatile-prefix: timed out, a volatile read was cached");
		Environment.Exit (10);
	}

	static int SpinTerminates ()
	{
		flag = false;

		Thread setter = new Thread (delegate () {
			Thread.Sleep (100);
			flag = true;
		});

		setter.Start ();
		while (!flag)
			;
		setter.Join ();
		return 0;
	}

	static void Publish ()
	{
		for (int i = 1; i <= Rounds; i++) {
			payload_a = i;
			payload_b = -i;
			ready = i;

			while (ready != 0)
				Thread.Yield ();
		}

		ready = -1;
	}

	static int Observes ()
	{
		Thread writer = new Thread (Publish);
		int result = 0;

		writer.Start ();

		for (;;) {
			int seen = ready;

			if (seen == 0) {
				Thread.Yield ();
				continue;
			}
			if (seen < 0)
				break;

			int a = payload_a;
			int b = payload_b;

			if ((a != seen || b != -seen) && result == 0) {
				Console.WriteLine ("volatile-prefix: ready={0}, payload=({1},{2})",
				                   seen, a, b);
				result = 2;
			}

			/* The handshake runs to the end either way; the writer is waiting on it. */
			ready = 0;
		}

		writer.Join ();
		return result;
	}

	public static int Main ()
	{
		Thread watchdog = new Thread (Watchdog);
		int failure;

		watchdog.IsBackground = true;
		watchdog.Start ();

		failure = SpinTerminates ();
		if (failure != 0)
			return failure;

		return Observes ();
	}
}
