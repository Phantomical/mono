using System;
using System.Threading;

/*
 * BeginInvoke and EndInvoke on a delegate over a reference type. Both are
 * implemented outside IL, so the body each one runs is a marshalling wrapper
 * that mini writes for the instantiation the caller named. An engine that
 * asks for the shared form instead gets an open wrapper, which mini refuses
 * as a containing type that is not fully instantiated.
 *
 * The loop runs past the tier-1 threshold, so the callers ask to promote and
 * the call sites reach the wrappers from compiled code as well, whenever a
 * promotion lands before the loop ends. Which engine runs the program is the
 * corpus arm's decision rather than this loop's.
 */

class Payload {
	public int n;

	public Payload (int n) { this.n = n; }
}

class DelegateBeginInvokeGeneric {
	static int seen;

	static void Handler (Payload p)
	{
		Interlocked.Add (ref seen, p.n);
	}

	static int Doubled (Payload p)
	{
		return p.n * 2;
	}

	static void Dispatch (Action<Payload> cb, Payload p)
	{
		IAsyncResult r = cb.BeginInvoke (p, null, null);
		cb.EndInvoke (r);
	}

	static int DispatchWithResult (Func<Payload, int> cb, Payload p)
	{
		IAsyncResult r = cb.BeginInvoke (p, null, null);
		return cb.EndInvoke (r);
	}

	public static int Main ()
	{
		Action<Payload> action = Handler;
		Func<Payload, int> func = Doubled;
		int total = 0;

		for (int i = 0; i < 200; i++) {
			Dispatch (action, new Payload (1));
			total += DispatchWithResult (func, new Payload (i));
		}

		if (seen != 200)
			return 1;

		/* 2 * (0 + 1 + ... + 199) */
		if (total != 199 * 200)
			return 2;

		return 0;
	}
}
