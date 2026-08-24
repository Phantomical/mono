using System;
using System.Runtime.Remoting;
using System.Threading;

// Marshals one fresh object from several threads at once, and checks that each
// thread gets a complete ObjRef. The threads behind the first one find the
// ServerIdentity it attached, so they all reach ServerIdentity.CreateObjRef ()
// together and take its cached ObjRef. That method has what an incomplete
// ObjRef costs.
//
// The runtime reaches this shape through AppDomain.DefaultDomain. Each fresh
// domain marshals the root domain's AppDomain object, and one thread for each
// domain arrives at the same identity.

class Server : MarshalByRefObject {
	// A null lease keeps this object's identity out of the lease manager.
	// Tracking a lease for each round's object buys the test nothing.
	public override object InitializeLifetimeService ()
	{
		return null;
	}
}

class RemotingObjRefRace {
	const int Threads = 8;
	const int Rounds = 400;

	static int halfBuilt;
	static int finished;

	// The round the workers can run, and the object they marshal in it. The
	// workers spin on the round to be inside Marshal at the same time.
	static volatile int openRound = -1;
	static volatile Server target;

	static void Race ()
	{
		for (int r = 0; r < Rounds; r++) {
			while (openRound != r)
				Thread.Yield ();

			ObjRef objRef = RemotingServices.Marshal (target);

			// A thread that took the cached ObjRef before the winner had
			// filled it in would see TypeInfo and URI unset. The winner
			// fills that same object in, so a later read finds it
			// complete.
			if (objRef.TypeInfo == null || objRef.URI == null)
				Interlocked.Increment (ref halfBuilt);

			Interlocked.Increment (ref finished);
		}
	}

	public static int Main ()
	{
		Thread[] workers = new Thread [Threads];
		for (int t = 0; t < Threads; t++) {
			workers [t] = new Thread (Race);
			workers [t].Start ();
		}

		for (int r = 0; r < Rounds; r++) {
			// One fresh object for each round. An object that is
			// marshalled already has its ObjRef, and every thread then
			// takes the same complete one.
			target = new Server ();
			openRound = r;

			while (Interlocked.CompareExchange (ref finished, 0, 0) < (r + 1) * Threads)
				Thread.Yield ();
		}

		foreach (Thread w in workers)
			w.Join ();

		if (halfBuilt != 0) {
			Console.WriteLine ("{0} threads got a half-built ObjRef", halfBuilt);
			return 1;
		}

		return 0;
	}
}
