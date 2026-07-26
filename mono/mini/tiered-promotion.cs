using System;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	//
	// Promotion-policy probe, backed by JIT-only internal calls registered in
	// mini-runtime.c. Lets the test assert the POLICY (a cold method stays tier
	// 0; a hot one is promoted), which semantic-only checks cannot catch.
	//
	static class Probe {
		// MONO_TIERED_CALL_THRESHOLD, or 0 when deferred promotion is off
		// (MONO_TIERED unset, or threshold 0 = eager). Policy assertions are only
		// meaningful when this is > 0.
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();

		// Recorded tier state of the method whose MonoMethod* is METHOD (obtained
		// from RuntimeMethodHandle.Value): 1 = promoted to tier 1, 0 = queued at
		// tier 0, 2 = tier-0 terminal (declined), -1 = no record, i.e. never
		// enqueued - the method never crossed the threshold and stayed cold.
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern int MethodState (IntPtr method);

		// TRUE once the background compile worker has armed METHOD's redirect
		// sled - i.e. its tier-0 entry now tail-jumps to tier 1 - as opposed to
		// MethodState () == 1, which only says the tier-1 body has been
		// published and looked-up calls will find it. FALSE while promotion is
		// still in flight, and always FALSE under eager (threshold 0) mode,
		// which has no counter block to carry a sled.
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool RedirectArmed (IntPtr method);
	}
}

//
// Functional test for deferred tier-1 promotion (MONO_TIERED_CALL_THRESHOLD).
//
// Every test method is correct regardless of whether its helpers are still at
// tier 0 or have been promoted to tier 1, so the suite passes on the classic
// JIT and under `MONO_TIERED=1 --llvm` at any threshold. Its purpose under
// tiering is to exercise the promotion path and prove it preserves semantics:
//
//   MONO_PATH=<class> MONO_TIERED=1 MONO_TIERED_CALL_THRESHOLD=20 \
//     mono --llvm --regression tiered-promotion.exe
//
// The helpers are marked NoInlining so their prologues (which carry the call
// counter) actually run on every call; an inlined helper would never count.
//
// Promotion runs on a background compile thread: crossing the threshold only
// enqueues the method and wakes that thread, so a test that checks Probe
// state right after driving a method past its threshold can observe the
// compile still in flight. Any assertion on MethodState ()/RedirectArmed ()
// therefore polls with WaitForState ()/WaitForRedirectArmed () rather than
// checking once - this is inherent to the design (see tiered.cpp), not a bug
// to work around by making the wait longer in one spot.
//
public class TieredPromotion {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (TieredPromotion), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Square (int x) { return x * x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long Fib (int n) { return n < 2 ? n : Fib (n - 1) + Fib (n - 2); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Cold (int a, int b) { return a * 3 - b; }

	// Square is entered 5000 times, far past any small threshold, so under a low
	// MONO_TIERED_CALL_THRESHOLD it is promoted partway through the loop. The sum
	// must be identical whether or not that happened.
	public static int test_0_hot_method_stable_across_promotion () {
		long sum = 0;
		for (int i = 0; i < 5000; i++)
			sum += Square (i % 100);
		// 50 full sweeps of k=0..99 of k*k: 50 * (99*100*199/6).
		long expected = 50L * (99 * 100 * 199 / 6);
		return sum == expected ? 0 : 1;
	}

	// Deep self-recursion enters Fib's prologue many times from a single
	// top-level call, so promotion can fire via the entry (not top-level-call)
	// count. The result must be stable.
	public static int test_0_recursive_method_promotion () {
		return Fib (25) == 75025 ? 0 : 1;
	}

	// A method entered only a handful of times stays tier 0 under a non-trivial
	// threshold (and is promoted eagerly at threshold 0); either way it must
	// compute the same value.
	public static int test_0_cold_method_correct () {
		int r = 0;
		r += Cold (10, 1);
		r += Cold (20, 2);
		r += Cold (30, 3);
		return r == (10 * 3 - 1) + (20 * 3 - 2) + (30 * 3 - 3) ? 0 : 1;
	}

	// -- Promotion-policy assertion (review NIT-3) --------------------------
	//
	// The tests above only prove semantics are preserved across promotion; they
	// pass even if the threshold were silently ignored. This one asserts the
	// POLICY itself: a hot method IS promoted and a below-threshold method is
	// NOT. It reads tier state through the MonoTests.Tiering.Probe internal
	// calls.
	//
	// Only meaningful when deferred promotion is active (threshold > 0). Under
	// the classic JIT (tiering off) or eager mode (threshold 0) the probe
	// reports "off" and there is no deferred policy to check, so the test
	// degrades to a no-op and still passes on every path and threshold.

	const int STATE_PROMOTED = 1;
	const int POLICY_COLD_CALLS = 3;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PolicyHot (int x) { return x + 1; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PolicyCold (int x) { return x - 1; }

	static IntPtr HandleOf (string name) {
		MethodInfo mi = typeof (TieredPromotion).GetMethod (
			name, BindingFlags.NonPublic | BindingFlags.Static);
		return mi.MethodHandle.Value;
	}

	// How many times to enter a helper to drive it past the configured
	// threshold. The margin either side of the crossing is what makes a
	// mid-promotion body observable, so this is deliberately more than the
	// bare threshold - but scaled to it, because the exception-heavy loops
	// below cost real time and --regression reruns them once per opt set.
	static int DriveCount () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	// Promotion happens on the background compile worker, so a crossing does
	// not mean "already promoted" - only "enqueued and the worker was just
	// woken". Poll rather than check once; a compile is normally done in low
	// milliseconds, so a generous 10s bound only ever matters if the policy
	// itself is broken (which is exactly what a timeout here should report).
	const int WAIT_TIMEOUT_MS = 10000;

	static bool WaitForState (IntPtr method, int wantState) {
		int waited = 0;
		while (MonoTests.Tiering.Probe.MethodState (method) != wantState) {
			if (waited >= WAIT_TIMEOUT_MS)
				return false;
			System.Threading.Thread.Sleep (5);
			waited += 5;
		}
		return true;
	}

	static bool WaitForRedirectArmed (IntPtr method) {
		int waited = 0;
		while (!MonoTests.Tiering.Probe.RedirectArmed (method)) {
			if (waited >= WAIT_TIMEOUT_MS)
				return false;
			System.Threading.Thread.Sleep (5);
			waited += 5;
		}
		return true;
	}

	public static int test_0_promotion_policy () {
		uint threshold = MonoTests.Tiering.Probe.Threshold ();
		if (threshold == 0)
			return 0;			// off / eager: no deferred policy to assert.

		// Drive PolicyHot far past any tested threshold; it must promote.
		long acc = 0;
		for (int i = 0; i < 5000; i++)
			acc += PolicyHot (i & 63);
		if (!WaitForState (HandleOf ("PolicyHot"), STATE_PROMOTED))
			return 1;			// a hot method was NOT promoted - policy regressed.

		// Enter PolicyCold only a handful of times. When the threshold is above
		// that count it can never cross, so it must NOT be promoted - and since
		// it never crosses, nothing ever enqueues it, so there is no compile in
		// flight to wait for here.
		int r = 0;
		for (int i = 0; i < POLICY_COLD_CALLS; i++)
			r += PolicyCold (i);
		if (threshold > POLICY_COLD_CALLS &&
		    MonoTests.Tiering.Probe.MethodState (HandleOf ("PolicyCold")) == STATE_PROMOTED)
			return 2;			// a cold method was promoted - threshold ignored.

		return (acc >= 0 && r != 0x7fffffff) ? 0 : 3;
	}

	// -- The redirect sled itself --------------------------------------------
	//
	// test_0_promotion_policy only proves the hash-swap happened (lookups now
	// resolve to tier 1). It does not prove that a call already in flight
	// through the method's OWN tier-0 entry point actually redirects - that is
	// what the sled (mono_arch_emit_prolog, point A) is for, and it is armed
	// separately, after the hash-swap, by the background worker. This asserts
	// that specifically, then keeps calling through the same (never
	// recompiled) call site and checks every result.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SledProbe (int x) { return x * 2 + 1; }

	public static int test_0_redirect_sled_fires () {
		uint threshold = MonoTests.Tiering.Probe.Threshold ();
		if (threshold == 0)
			return 0;			// eager mode has no counter block, hence no sled.

		IntPtr h = HandleOf ("SledProbe");
		for (int i = 0; i < 10000; i++)
			SledProbe (i % 17);

		if (!WaitForRedirectArmed (h))
			return 1;			// promoted, per test_0_promotion_policy's check
						// elsewhere, but the sled never armed.

		// The sled is now armed. Every one of these calls must redirect to
		// tier 1 on entry - the call site itself was fixed when this method
		// was first compiled and is never revisited - and every result must
		// still be correct.
		for (int i = 0; i < 1000; i++) {
			int x = i % 17;
			if (SledProbe (x) != x * 2 + 1)
				return 2;
		}
		return 0;
	}

	// -- Exception thrown through a tiered method ----------------------------
	//
	// PassThrough is the method under test/promotion; it has no try/catch of
	// its own, so an exception raised by Thrower (its callee) propagates
	// STRAIGHT THROUGH its frame to the catch below. This is exactly the shape
	// that would break if the entry-redirect sled (mono_arch_emit_prolog,
	// point A) shifted a single byte of the prologue's unwind info: the
	// unwinder has to walk through PassThrough's frame - using whatever
	// unwind info its CURRENT body (tier 0, mid-redirect, or tier 1) published
	// - to reach the handler here. Run once before promotion is likely and
	// once after the sled is confirmed armed, so both a tier-0 and a tier-1
	// frame get walked through.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Thrower (int x) {
		if (x < 0)
			throw new InvalidOperationException ("neg");
		return x;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PassThrough (int x) {
		return Thrower (x) + 1;
	}

	public static int test_0_exception_through_tiered_method () {
		uint threshold = MonoTests.Tiering.Probe.Threshold ();
		IntPtr h = HandleOf ("PassThrough");

		// Mid-promotion: throws on every 7th call while the driving loop is
		// also what pushes PassThrough's counter across the threshold, so the
		// throw and the crossing race each other.
		int caught = 0;
		for (int i = 0, n = DriveCount (); i < n; i++) {
			try {
				int x = (i % 7 == 0) ? -1 : (i % 1000);
				int r = PassThrough (x);
				if (x >= 0 && r != x + 1)
					return 1;
			} catch (InvalidOperationException) {
				caught++;
			}
		}
		if (caught == 0)
			return 2;			// the throwing branch never ran - test is broken,
						// not a pass.

		if (threshold != 0 && !WaitForRedirectArmed (h))
			return 3;

		// After promotion: every call here should be entirely tier 1 (sled
		// confirmed armed above), including the ones that throw.
		int caught2 = 0;
		for (int i = 0; i < 200; i++) {
			try {
				int x = (i % 3 == 0) ? -1 : i;
				int r = PassThrough (x);
				if (x >= 0 && r != x + 1)
					return 4;
			} catch (InvalidOperationException) {
				caught2++;
			}
		}
		if (caught2 == 0)
			return 5;
		return 0;
	}

	// -- Concurrent callers across promotion ----------------------------------
	//
	// Several threads hammer the same method while it promotes underneath
	// them; the redirect slot (MiniTieredCounter.tier1_entry) flips from NULL
	// to a real code pointer exactly once, with a single release-store
	// (mono_atomic_store_ptr in the worker) and a plain-load read (the
	// prologue) - this is what proves that flip is safe to observe
	// concurrently: no caller may ever see a torn pointer, a crash, or a
	// wrong result, regardless of which side of the flip its particular call
	// lands on.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ConcurrentHot (int x) { return x * x + 1; }

	public static int test_0_concurrent_callers_across_promotion () {
		uint threshold = MonoTests.Tiering.Probe.Threshold ();
		IntPtr h = HandleOf ("ConcurrentHot");

		const int NUM_THREADS = 4;
		const int ITERS = 20000;
		bool[] ok = new bool[NUM_THREADS];
		var threads = new System.Threading.Thread[NUM_THREADS];

		for (int t = 0; t < NUM_THREADS; t++) {
			int idx = t;
			threads[t] = new System.Threading.Thread (() => {
				bool good = true;
				for (int i = 0; i < ITERS; i++) {
					int x = i % 200;
					if (ConcurrentHot (x) != x * x + 1) {
						good = false;
						break;
					}
				}
				ok[idx] = good;
			});
		}

		foreach (var th in threads)
			th.Start ();
		foreach (var th in threads)
			th.Join ();

		foreach (bool o in ok)
			if (!o)
				return 1;		// a caller saw a wrong result while the slot
						// flipped underneath it.

		// NUM_THREADS * ITERS = 80000 calls, comfortably past every tested
		// threshold, so this must have promoted.
		if (threshold != 0 && !WaitForState (h, STATE_PROMOTED))
			return 2;
		return 0;
	}

	// -- A cctor already run at tier 0, not re-run by promotion ---------------
	//
	// The original intent here was a cctor still PENDING when promotion
	// happens. That does not occur with a plain static field reference: even
	// HotWithPendingCctor's very first (tier-0) compile carries
	// JIT_FLAG_RUN_CCTORS (mono_jit_compile_method_inner_1 () sets it
	// unconditionally, nothing to do with tiering), so PendingCctor's
	// initializer already runs during THAT compile, on the calling thread,
	// long before any promotion. What tiering must not do is run it a SECOND
	// time: mini_tiered_promote () recompiles this same method from scratch
	// for tier 1, and if run_cctors were not correctly threaded through as
	// FALSE for the background worker, that recompile could re-touch
	// PendingCctor's vtable and re-run its initializer - on the worker
	// thread, which is exactly the invariant this guards.

	static class PendingCctor {
		public static int RunCount;
		public static int RunThreadId = -1;

		static PendingCctor () {
			RunCount++;
			RunThreadId = System.Threading.Thread.CurrentThread.ManagedThreadId;
		}

		public static int Value = 99;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int HotWithPendingCctor (bool touch) {
		if (touch)
			return PendingCctor.Value;
		return 0;
	}

	public static int test_0_cctor_not_rerun_by_promotion () {
		uint threshold = MonoTests.Tiering.Probe.Threshold ();
		IntPtr h = HandleOf ("HotWithPendingCctor");
		int mainThreadId = System.Threading.Thread.CurrentThread.ManagedThreadId;

		for (int i = 0; i < 5000; i++)
			HotWithPendingCctor (false);

		if (PendingCctor.RunCount != 1)
			return 1;			// classic tier-0 semantics assumption (see
						// above) doesn't hold - not yet even a tiering
						// question.
		if (PendingCctor.RunThreadId != mainThreadId)
			return 2;

		if (threshold != 0 && !WaitForRedirectArmed (h))
			return 3;			// never promoted - nothing to have re-run
						// the cctor to guard against.

		// Promotion - a full recompile of HotWithPendingCctor, on the
		// worker, with run_cctors = FALSE - has now happened (or, at
		// threshold 0, happened eagerly on the tier-0 publish path, still
		// with run_cctors plumbed through the same way). Either way it must
		// not have re-run the cctor.
		if (PendingCctor.RunCount != 1)
			return 4;			// ran again - re-running an already-run
						// cctor is exactly the defect the run_cctors
						// fork exists to prevent.

		int v = HotWithPendingCctor (true);
		if (v != 99)
			return 5;
		if (PendingCctor.RunCount != 1)
			return 6;
		if (PendingCctor.RunThreadId != mainThreadId)
			return 7;
		return 0;
	}

	// -- AggressiveInlining callee's cctor not RE-RUN by promotion ------------
	//
	// A different site from the one above: method-to-ir.c's inlining-decision
	// function has a dedicated branch for [MethodImplOptions.AggressiveInlining]
	// callees that forces their cctor to run right there, specifically so the
	// callee can be safely inlined - gated on run_cctors (like the site above)
	// rather than unconditionally.
	//
	// This test does NOT reach that branch's !vtable->initialized arm: by the
	// time HotCallerOfAggressive is promotable it has already had a tier-0
	// compile, which - always carrying JIT_FLAG_RUN_CCTORS - already forced
	// AggressiveCallee's class through this same branch and initialized it.
	// Tier-0 and tier-1 see identical inlining candidates, so when the worker
	// reconsiders inlining AggressiveCallee, the class is already initialized
	// and the branch is a no-op either way. What this DOES verify, and is the
	// real property at stake: promotion (a from-scratch recompile with
	// run_cctors = FALSE) must not RE-RUN an already-run cctor - RunCount
	// must stay exactly 1, on the original (mutator) thread, all the way
	// through. See the comment on that branch in method-to-ir.c for why its
	// !vtable->initialized arm is defensive rather than exercised here.

	static class AggressivePendingCctor {
		public static int RunCount;
		public static int RunThreadId = -1;

		static AggressivePendingCctor () {
			RunCount++;
			RunThreadId = System.Threading.Thread.CurrentThread.ManagedThreadId;
		}

		public static int Value = 7;
	}

	[MethodImpl (MethodImplOptions.AggressiveInlining)]
	static int AggressiveCallee (int x) {
		return x + AggressivePendingCctor.Value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int HotCallerOfAggressive (int x) {
		return AggressiveCallee (x);
	}

	public static int test_0_aggressive_inlining_cctor_not_run_on_worker () {
		uint threshold = MonoTests.Tiering.Probe.Threshold ();
		IntPtr h = HandleOf ("HotCallerOfAggressive");
		int mainThreadId = System.Threading.Thread.CurrentThread.ManagedThreadId;

		// HotCallerOfAggressive's own first (tier-0) compile already reaches
		// AggressiveCallee - inlined or not - so AggressivePendingCctor's
		// cctor already runs here, on this thread, well before promotion;
		// same classic eager-cctor-at-compile-time behavior as
		// test_0_cctor_not_rerun_by_promotion above. What must not happen is
		// a SECOND run when the worker recompiles HotCallerOfAggressive for
		// tier 1 and its IR generation reconsiders inlining AggressiveCallee.
		for (int i = 0; i < 5000; i++) {
			int x = i % 1000;
			if (HotCallerOfAggressive (x) != x + 7)
				return 1;
		}

		if (AggressivePendingCctor.RunCount != 1)
			return 2;
		if (AggressivePendingCctor.RunThreadId != mainThreadId)
			return 3;

		if (threshold != 0 && !WaitForRedirectArmed (h))
			return 4;

		// Promotion - recompiling HotCallerOfAggressive on the worker, with
		// run_cctors = FALSE, reconsidering the AggressiveInlining callee -
		// has now happened. It must not have re-run the cctor.
		if (AggressivePendingCctor.RunCount != 1)
			return 5;

		for (int i = 0; i < 200; i++) {
			int x = i % 1000;
			if (HotCallerOfAggressive (x) != x + 7)
				return 6;
		}
		if (AggressivePendingCctor.RunCount != 1)
			return 7;
		if (AggressivePendingCctor.RunThreadId != mainThreadId)
			return 8;
		return 0;
	}

	// -- Same-frame handler chains across promotion ---------------------------
	//
	// One throw, several handlers from the SAME frame. The runtime does not
	// deliver those in one go: it enters the frame's landing pad, runs the
	// innermost cleanup, and the cleanup's resume trampoline hands the frame
	// back so the runtime can come in again for the next clause out. Every one
	// of these shapes has each handler write the same local and the next one
	// read it, so if tier 1 loses track of that re-entry - the LLVM IR has to
	// carry an honest edge from the resume out to where control really goes, or
	// the optimizer folds the cleanup's stores away as dead - the answer comes
	// back short by exactly the missing handler's contribution. No crash, just a
	// wrong number, which is why these are value checks rather than "did it
	// throw" checks.
	//
	// The Nop () in each cleanup body gives the enclosing clause a protected
	// call of its own, so the chain is exercised with a landing pad at every
	// level rather than only at the innermost one.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Boom () { throw new InvalidOperationException ("boom"); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void BoomArg () { throw new ArgumentException ("arg"); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Nop () { }

	// Two cleanups then a catch, all in this frame: finally, finally, catch.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CleanupChain3 () {
		int s = 0;
		try {
			try {
				try {
					s += 1;
					Boom ();
					s += 2;
				} finally {
					Nop ();
					s += 10;
				}
			} finally {
				Nop ();
				s += 100;
			}
		} catch (Exception) {
			s += 10000;
		}
		return s;
	}

	// One link longer, to check the chain is not capped at a single re-entry.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CleanupChain4 () {
		int s = 0;
		try {
			try {
				try {
					try {
						s += 1;
						Boom ();
					} finally {
						Nop ();
						s += 10;
					}
				} finally {
					Nop ();
					s += 100;
				}
			} finally {
				Nop ();
				s += 1000;
			}
		} catch (Exception) {
			s += 100000;
		}
		return s;
	}

	// A catch nested inside a cleanup's try region. The inner catch does not
	// match, so the walk passes through it, runs the finally, and only then
	// reaches the outer catch.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CatchInsideCleanup () {
		int s = 0;
		try {
			try {
				try {
					s += 1;
					Boom ();
				} catch (ArgumentException) {
					s += 10;
				}
			} finally {
				Nop ();
				s += 100;
			}
		} catch (Exception) {
			s += 10000;
		}
		return s;
	}

	// The same shape with the inner catch MATCHING, so it swallows the throw and
	// the finally runs on the ordinary leave path instead of on a resume.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CatchInsideCleanupMatched () {
		int s = 0;
		try {
			try {
				try {
					s += 1;
					BoomArg ();
				} catch (ArgumentException) {
					s += 10;
				}
			} finally {
				Nop ();
				s += 100;
			}
		} catch (Exception) {
			s += 10000;
		}
		return s;
	}

	// A cleanup enclosed by SIBLING catches: the re-entry has to pick between
	// two clauses over one try region by selector, not just fall into the first.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CleanupUnderSiblingCatches (bool arg) {
		int s = 0;
		try {
			try {
				s += 1;
				if (arg)
					BoomArg ();
				else
					Boom ();
			} finally {
				Nop ();
				s += 10;
			}
		} catch (ArgumentException) {
			s += 100;
		} catch (InvalidOperationException) {
			s += 1000;
		}
		return s;
	}

	// The throw happens in the MIDDLE try, outside the innermost clause, so the
	// innermost cleanup must NOT run and the chain starts one level out.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CleanupChainSkipInnermost () {
		int s = 0;
		try {
			try {
				try {
					s += 1;
					Nop ();
				} finally {
					Nop ();
					s += 10;
				}
				Boom ();
				s += 2;
			} finally {
				Nop ();
				s += 100;
			}
		} catch (Exception) {
			s += 10000;
		}
		return s;
	}

	// The chain inside a loop, so the optimizer has real phis to fold across
	// each re-entry rather than a single straight-line path.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CleanupChainInLoop (int n) {
		int s = 0;
		for (int i = 0; i < n; i++) {
			try {
				try {
					try {
						s += 1;
						if ((i % 2) == 0)
							Boom ();
						s += 2;
					} finally {
						Nop ();
						s += 10;
					}
				} finally {
					Nop ();
					s += 100;
				}
			} catch (Exception) {
				s += 10000;
			}
		}
		return s;
	}

	// The second cleanup throws a NEW exception, which has to abandon the one
	// already in flight and leave the frame instead of continuing the chain.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CleanupThrowsFromCleanup () {
		int s = 0;
		try {
			try {
				s += 1;
				Boom ();
			} finally {
				Nop ();
				s += 10;
			}
		} finally {
			Nop ();
			s += 100;
			BoomArg ();
		}
		return s;
	}

	public static int test_0_same_frame_handler_chain () {
		uint threshold = MonoTests.Tiering.Probe.Threshold ();
		IntPtr h = HandleOf ("CleanupChain3");

		// Drive every shape well past any threshold, checking each call: the
		// tier-0 answers, the mid-promotion ones and the tier-1 ones all have to
		// agree, so a body that only goes wrong once promoted still fails here.
		for (int i = 0, n = DriveCount (); i < n; i++) {
			if (CleanupChain3 () != 10111)
				return 1;
			if (CleanupChain4 () != 101111)
				return 2;
			if (CatchInsideCleanup () != 10101)
				return 3;
			if (CatchInsideCleanupMatched () != 111)
				return 4;
			if (CleanupUnderSiblingCatches (true) != 111)
				return 5;
			if (CleanupUnderSiblingCatches (false) != 1011)
				return 6;
			if (CleanupChainSkipInnermost () != 10111)
				return 7;
		}

		if (threshold != 0 && !WaitForRedirectArmed (h))
			return 8;

		// Same again with the sled confirmed armed, so these calls are tier 1
		// end to end.
		for (int i = 0; i < 200; i++) {
			if (CleanupChain3 () != 10111)
				return 9;
			if (CleanupChain4 () != 101111)
				return 10;
			if (CatchInsideCleanup () != 10101)
				return 11;
			if (CatchInsideCleanupMatched () != 111)
				return 12;
			if (CleanupUnderSiblingCatches (true) != 111)
				return 13;
			if (CleanupUnderSiblingCatches (false) != 1011)
				return 14;
			if (CleanupChainSkipInnermost () != 10111)
				return 15;
		}
		return 0;
	}

	public static int test_0_same_frame_handler_chain_in_loop () {
		IntPtr h = HandleOf ("CleanupChainInLoop");
		uint threshold = MonoTests.Tiering.Probe.Threshold ();

		// n throwing iterations contribute 10111 each and n non-throwing ones
		// 113 each (1 + 2 in the try, then both cleanups).
		for (int i = 0, n = DriveCount (); i < n; i++) {
			if (CleanupChainInLoop (1) != 10111)
				return 1;
			if (CleanupChainInLoop (2) != 10111 + 113)
				return 2;
			if (CleanupChainInLoop (7) != 4 * 10111 + 3 * 113)
				return 3;
		}

		if (threshold != 0 && !WaitForRedirectArmed (h))
			return 4;

		if (CleanupChainInLoop (7) != 4 * 10111 + 3 * 113)
			return 5;
		return 0;
	}

	// The InvalidOperationException this drives has no handler anywhere on the
	// stack - only the ArgumentException that the outer cleanup throws in its
	// place does. So the runtime's first pass finds nothing, reports it as
	// unhandled, and only then runs the second pass, where the cleanup replaces
	// it with the exception that is actually caught. That report is expected
	// here, but it prints a freshly built managed backtrace per iteration,
	// which costs far more than the exception itself. Swallow it for the
	// duration of the test - narrowly, so a genuinely unhandled exception
	// anywhere else in the suite still gets reported.
	public static int test_0_exception_from_within_cleanup_chain () {
		IntPtr h = HandleOf ("CleanupThrowsFromCleanup");
		uint threshold = MonoTests.Tiering.Probe.Threshold ();
		int caught = 0;
		int n = DriveCount ();

		UnhandledExceptionEventHandler expected = (sender, e) => { };
		AppDomain.CurrentDomain.UnhandledException += expected;
		try {
			for (int i = 0; i < n; i++) {
				try {
					CleanupThrowsFromCleanup ();
					return 1;		// it always throws
				} catch (ArgumentException) {
					caught++;
				}
			}
			if (caught != n)
				return 2;

			if (threshold != 0 && !WaitForRedirectArmed (h))
				return 3;

			try {
				CleanupThrowsFromCleanup ();
				return 4;
			} catch (ArgumentException) {
			}
			return 0;
		} finally {
			AppDomain.CurrentDomain.UnhandledException -= expected;
		}
	}

	// -- Clauses whose protected region holds no call ------------------------
	//
	// A try region with no call in it gets no invoke, so its clause gets no
	// landing pad: the handler is reachable only through OP_CALL_HANDLER on the
	// leave path. That is the common shape once tier 0 has inlined a small body
	// away - every foreach over a struct enumerator ends up here - so these
	// check that the cleanup still runs, and runs once, with nothing for the
	// unwinder to find.
	//
	// The throwing variants are the other half of the pair: a region that only
	// LOOKS call-free in the IL still gets an invoke, because implicit
	// exceptions and explicit throws are emitted as calls. Their answers must
	// match tier 0's too.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CallFreeFinally () {
		int x = 0;
		try {
			for (int i = 0; i < 4; i++)
				x += i;
		} finally {
			x += 100;
		}
		return x;
	}

	// Cleanup reached by continue (a leave out of the try) as well as by
	// falling off the end, so it has to run once per iteration either way.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CallFreeFinallyLoop (int n) {
		int x = 0;
		for (int i = 0; i < n; i++) {
			try {
				if (i == 2)
					continue;
				x += i;
			} finally {
				x += 1000;
			}
		}
		return x;
	}

	// Cleanup reached by a return out of the try.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CallFreeFinallyReturn (int n, int[] log) {
		try {
			if (n > 0)
				return n + 1;
			return 5;
		} finally {
			log [0]++;
		}
	}

	// Call-free in the IL, but the null deref is an implicit exception - the
	// translator emits it as a throw call, so the region does carry an invoke.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CallFreeThrowFinally (int[] a) {
		int x = 0;
		try {
			x += a [0];
			x += a [1];
		} catch (NullReferenceException) {
			x += 10;
		} finally {
			x += 100;
		}
		return x;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CallFreeExplicitThrow (int n) {
		int x = 0;
		try {
			if (n < 0)
				throw new ArgumentException ("neg");
			x += n;
		} catch (ArgumentException) {
			x += 10;
		} finally {
			x += 100;
		}
		return x;
	}

	// A struct enumerator whose MoveNext/Current/Dispose all inline away, which
	// is what leaves the foreach's try/finally with a call-free protected
	// region in the first place.
	struct Counter {
		int i, n;
		public Counter (int n) { this.i = -1; this.n = n; }
		public bool MoveNext () { i++; return i < n; }
		public int Current { get { return i; } }
		public void Dispose () { }
	}

	struct Counted {
		int n;
		public Counted (int n) { this.n = n; }
		public Counter GetEnumerator () { return new Counter (n); }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int StructEnumeratorForeach (int n) {
		int x = 0;
		foreach (int v in new Counted (n))
			x += v;
		return x;
	}

	public static int test_0_call_free_protected_region () {
		uint threshold = MonoTests.Tiering.Probe.Threshold ();
		IntPtr h = HandleOf ("CallFreeFinally");
		int[] log = new int [1];
		int n = DriveCount ();

		for (int i = 0; i < n; i++) {
			if (CallFreeFinally () != 106)
				return 1;
			if (CallFreeFinallyLoop (5) != 5008)
				return 2;
			if (CallFreeFinallyReturn (3, log) != 4)
				return 3;
			if (CallFreeFinallyReturn (0, log) != 5)
				return 4;
			if (CallFreeThrowFinally (new int[] { 1, 2 }) != 103)
				return 5;
			if (CallFreeThrowFinally (null) != 110)
				return 6;
			if (CallFreeExplicitThrow (7) != 107)
				return 7;
			if (CallFreeExplicitThrow (-1) != 110)
				return 8;
			if (StructEnumeratorForeach (5) != 10)
				return 9;
		}

		// Each cleanup ran exactly once per call, neither skipped nor doubled.
		if (log [0] != 2 * n)
			return 10;

		if (threshold != 0 && !WaitForRedirectArmed (h))
			return 11;

		// Same again with the sled armed, so these calls are tier 1 end to end.
		for (int i = 0; i < 200; i++) {
			if (CallFreeFinally () != 106)
				return 12;
			if (CallFreeFinallyLoop (5) != 5008)
				return 13;
			if (CallFreeFinallyReturn (3, log) != 4)
				return 14;
			if (CallFreeThrowFinally (null) != 110)
				return 15;
			if (CallFreeExplicitThrow (-1) != 110)
				return 16;
			if (StructEnumeratorForeach (5) != 10)
				return 17;
		}
		if (log [0] != 2 * n + 200)
			return 18;
		return 0;
	}
}
