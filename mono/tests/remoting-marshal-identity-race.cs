using System;
using System.Runtime.Remoting;
using System.Threading;

// Marshals one fresh object from several threads at once and counts the
// identities that RemotingServices registers for it. A registration on this
// path asks the object for a lease, so a count of the lease requests is a
// count of the identities. The object must end a round with one.

class Server : MarshalByRefObject {
	public static int leases;

	// A null lease keeps this object's identity out of the lease manager. The
	// test counts the request for a lease, not the lease itself.
	public override object InitializeLifetimeService ()
	{
		Interlocked.Increment (ref leases);
		return null;
	}
}

class RemotingMarshalIdentityRace {
	const int Threads = 8;
	const int Rounds = 400;

	static int finished;
	static int extraIdentities;

	// The round the workers can run, and the object they marshal in it. The
	// workers spin on the round to be inside Marshal at the same time.
	static volatile int openRound = -1;
	static volatile Server target;

	static void Race ()
	{
		for (int r = 0; r < Rounds; r++) {
			while (openRound != r)
				Thread.Yield ();

			RemotingServices.Marshal (target);
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

		int reported = 0;

		for (int r = 0; r < Rounds; r++) {
			// One fresh object for each round. A thread that marshals an
			// object with an identity already builds none. Every worker is
			// outside Marshal here, so the reset cannot lose a lease request.
			Interlocked.Exchange (ref Server.leases, 0);
			target = new Server ();
			openRound = r;

			while (Interlocked.CompareExchange (ref finished, 0, 0) < (r + 1) * Threads)
				Thread.Yield ();

			int leases = Interlocked.CompareExchange (ref Server.leases, 0, 0);
			if (leases != 1) {
				if (reported++ < 10)
					Console.WriteLine ("round {0} built {1} identities", r, leases);
				extraIdentities += leases - 1;
			}
		}

		foreach (Thread w in workers)
			w.Join ();

		if (extraIdentities != 0) {
			Console.WriteLine ("{0} identities beyond the one the object holds", extraIdentities);
			return 1;
		}

		return 0;
	}
}
