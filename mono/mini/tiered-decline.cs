using System;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	// Duplicated from tiered-promotion.cs rather than shared: these two
	// corpora are separate assemblies (this one needs a process-wide
	// MONO_LLVM_METHOD setting the other must NOT run under), and the probe
	// is a handful of internal-call declarations, not worth a shared-assembly
	// dependency between two single-purpose regression executables.
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();

		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern int MethodState (IntPtr method);

		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool RedirectArmed (IntPtr method);
	}
}

//
// Functional test for a DECLINED tier-1 promotion: the backend refuses the
// method (any of its gates - EH-filter-clause, gshared, save_lmf, GC
// safepoints), so it must stay tier 0 forever, keep running correctly, and
// never re-dispatch.
//
// Forcing an actual decline from portable C# is the hard part: this backend
// supports ordinary try/catch/finally/gshared, so none of those trigger it.
// The reliable lever is MONO_LLVM_METHOD (translator.cpp) - when set, EVERY
// method not matching its filter is excluded from the LLVM path, which is
// exactly a decline as far as mini_tiered_promote () is concerned. That makes
// this a whole-PROCESS setting, so it runs as its own corpus/invocation
// rather than folding into tiered-promotion.exe's suite:
//
//   MONO_LLVM_METHOD='TieredDecline:NeverMatchesAnything' \
//     MONO_TIERED=1 MONO_TIERED_CALL_THRESHOLD=20 \
//     mono --regression tiered-decline.exe
//
public class TieredDecline {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (TieredDecline), args);
	}

	const int STATE_TIER0_TERMINAL = 2;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int DeclinedHot (int x) { return x * 3 + 1; }

	static IntPtr HandleOf (string name) {
		MethodInfo mi = typeof (TieredDecline).GetMethod (
			name, BindingFlags.NonPublic | BindingFlags.Static);
		return mi.MethodHandle.Value;
	}

	static bool WaitForState (IntPtr method, int wantState, int timeoutMs) {
		int waited = 0;
		while (MonoTests.Tiering.Probe.MethodState (method) != wantState) {
			if (waited >= timeoutMs)
				return false;
			System.Threading.Thread.Sleep (5);
			waited += 5;
		}
		return true;
	}

	public static int test_0_declined_promotion_stays_correct () {
		uint threshold = MonoTests.Tiering.Probe.Threshold ();
		IntPtr h = HandleOf ("DeclinedHot");

		// Drive well past any tested threshold; DeclinedHot must be enqueued
		// (the call-count counter does not know or care that it will decline)
		// and correctness must hold throughout regardless.
		for (int i = 0; i < 5000; i++) {
			int x = i % 100;
			if (DeclinedHot (x) != x * 3 + 1)
				return 1;
		}

		if (threshold != 0) {
			// The worker still has to actually run the compile and get the
			// decline back before the state settles - poll for it.
			if (!WaitForState (h, STATE_TIER0_TERMINAL, 10000))
				return 2;			// never declined - is MONO_LLVM_METHOD
							// actually excluding this method?
			if (MonoTests.Tiering.Probe.RedirectArmed (h))
				return 3;			// declined but the sled got armed anyway.
		}

		// Keep calling well past the decline settling: must stay correct, no
		// crash, and (structurally, via the saturating counter + settled CAS
		// guard - see tiered.cpp) no re-dispatch storm.
		for (int i = 0; i < 3000; i++) {
			int x = i % 100;
			if (DeclinedHot (x) != x * 3 + 1)
				return 4;
		}

		if (threshold != 0 && MonoTests.Tiering.Probe.MethodState (h) != STATE_TIER0_TERMINAL)
			return 5;			// somehow un-settled after the fact.

		return 0;
	}
}
