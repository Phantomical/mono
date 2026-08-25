using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

/*
 * The work a tier-1 body does, as the half of the tier-2 counter that a count of
 * calls does not reach.
 *
 * HeavyKernel () and LightKernel () have the same body and take the same number
 * of calls, far below what the entry weight alone spends. They differ in the
 * turns of one loop. So the calls each of them charges are equal, and the work
 * is the only thing that can take one of them to tier 2 and leave the other at
 * tier 1. That pair is this test's control, and it holds inside one process
 * rather than across two runs of it.
 *
 * What says the tier-2 body is the one running is the stack trace. Probe () has
 * a branch, so the shape-test pre-pass declines it and only the tier-2 cost
 * model folds it in. A folded body owns no code: its frame reports the native
 * offset of the call site it was folded at, and the same helper called for real
 * reports an offset into its own body.
 *
 * Unwinder () and UnwinderLoop () are the third way out of a body. They reach no
 * ret and throw nothing themselves: every call leaves through the exception of a
 * callee, which finds no clause here and unwinds through the frame. Unwinder ()
 * takes enough calls to promote on what its entry charges. UnwinderLoop () takes
 * far too few for that and spends the rest of its counter in a long loop, so only
 * the turns it made before the exception can take it to tier 2.
 *
 * The suite registers this source twice. MONO_LLVM_JIT_TIER2_THRESHOLD is a
 * number one arm reaches and is zero in the other, which turns automatic
 * promotion off. The test reads the variable and asserts the arm it is in.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Program {
	/* MonoTier::tier1, as PromoteNow takes it. */
	const int tier1 = 2;

	/* Turns of the loop in one call of each kernel. */
	const int heavy = 100000;
	const int light = 10;

	/*
	 * Calls each kernel takes before the test looks at the tier. The entry
	 * weight is five thousand, so both kernels charge about three hundred
	 * thousand for their calls, and the threshold the suite sets is ten million.
	 * Neither of them promotes on the calls alone.
	 */
	const int calls = 64;

	/*
	 * Calls of Tiny (), which has no loop and spends about the entry weight in
	 * each of them. Four thousand takes it past the same ten million, so it
	 * promotes on its calls where the work in it reaches nothing.
	 */
	const int tiny_calls = 4000;

	/*
	 * Turns of the loop in Unwinder (), and calls of it. The loop is short, so the
	 * call site behind it keeps a block count near the count of the calls, and the
	 * cost model reads that site as hot enough to fold.
	 *
	 * A call charges the entry weight and the blocks no loop holds, which is a
	 * little past five thousand. Four thousand calls take that past the same ten
	 * million.
	 */
	const int unwind_turns = 4;
	const int unwind_calls = 4000;

	/*
	 * Turns of the loop in UnwinderLoop (), and calls of it. Sixty-four calls
	 * charge about three hundred thousand at the entry, which is well short of the
	 * same ten million. The loop carries the rest. The throw that ends each call
	 * unwinds out of the frame without reaching the ret, so the turns reach the
	 * counter only through the tier counter's pad.
	 */
	const int unwind_loop_turns = 100000;
	const int unwind_loop_calls = 64;

	/* Whether Probe () had a frame in the last trace, and where its code was. */
	static bool saw_probe, probe_runs_inside_kernel;

	// A branch, so the shape-test pre-pass declines this body and the tier-2
	// cost model is the only thing that folds it in.
	static void Probe (string what, bool yes)
	{
		if (yes)
			throw new InvalidOperationException (what);
	}

	/*
	 * Whether the frame reported for Probe () covers the same code as its
	 * caller's.
	 *
	 * A folded body has no code of its own, so the frame built for it reports
	 * the call site it was folded at. That is the same native offset the caller's
	 * own frame reports. A helper that was really called runs in its own body and
	 * answers with an offset into that.
	 */
	static bool RunsInsideKernel (Exception e, string kernel)
	{
		StackTrace st = new StackTrace (e, false);
		int in_probe = -1, in_kernel = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null || m.DeclaringType.Name != "Program")
				continue;
			if (m.Name == "Probe")
				in_probe = f.GetNativeOffset ();
			if (m.Name == kernel)
				in_kernel = f.GetNativeOffset ();
		}

		return in_probe >= 0 && in_probe == in_kernel;
	}

	/*
	 * The two kernels are one body written twice, so that the pair differs in
	 * the turns of the loop and in nothing else. Neither may fold into the other
	 * or into Observe (), because a folded kernel counts against its caller.
	 *
	 * Probe () is called from inside the loop rather than after it. The cost
	 * model ranks a call site by the block count the profile gave it, and a site
	 * beside a loop this long reads cold whatever the loop costs. Inside it the
	 * site is as hot as the loop.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int HeavyKernel (int n, bool throwing)
	{
		int total = 0;

		try {
			for (int i = 0; i < n; ++i) {
				total += i * 3 + (i & 7);
				Probe ("probe", throwing);
			}
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_probe = true;
			probe_runs_inside_kernel = RunsInsideKernel (e, "HeavyKernel");
		}

		return total;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int LightKernel (int n, bool throwing)
	{
		int total = 0;

		try {
			for (int i = 0; i < n; ++i) {
				total += i * 3 + (i & 7);
				Probe ("probe", throwing);
			}
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_probe = true;
			probe_runs_inside_kernel = RunsInsideKernel (e, "LightKernel");
		}

		return total;
	}

	/*
	 * A body with no loop, which spends about the entry weight in a call and
	 * almost nothing else. It is the shape a count of work alone never reaches,
	 * and SharpChess is full of it: a property getter of a few instructions,
	 * called very often, whose tier-2 payoff is being folded into its callers.
	 *
	 * Probe () throws only for a negative argument, so the calls that spend the
	 * counter cost what the body costs and the last one reads the tier.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Tiny (int x)
	{
		try {
			Probe ("tiny", x < 0);
		} catch (InvalidOperationException e) {
			saw_probe = true;
			probe_runs_inside_kernel = RunsInsideKernel (e, "Tiny");
			return 0;
		}

		return x * 3 + 1;
	}

	/*
	 * A body that leaves its frame only through the exception of a callee.
	 * Probe () throws on every call, and no clause here catches it, so the ret
	 * below is dead at run time.
	 *
	 * The loop puts this body on the accumulator path, and the pad is the only exit
	 * that charges the accumulator, because no call reaches the ret. Four turns are
	 * far less than what the entry charges, so this case promotes on the calls.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Unwinder (int n)
	{
		int total = 0;

		for (int i = 0; i < n; ++i)
			total += i * 3 + (i & 7);

		Probe ("unwind", total >= 0);
		return total;
	}

	/*
	 * Unwinder () with the call moved inside the loop, and with too few calls of it
	 * to promote.
	 *
	 * Probe () throws on the last turn, so the site is as hot as the loop, which is
	 * what the cost model reads to fold it. The frame still leaves through a
	 * callee's exception.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UnwinderLoop (int n)
	{
		int total = 0;

		for (int i = 0; i < n; ++i) {
			total += i * 3 + (i & 7);
			Probe ("unwind loop", i == n - 1);
		}

		return total;
	}

	/* One call that throws, and what its trace said about the tier. */
	static bool Observe (bool is_heavy)
	{
		saw_probe = probe_runs_inside_kernel = false;

		if (is_heavy)
			HeavyKernel (heavy, true);
		else
			LightKernel (light, true);

		if (!saw_probe)
			throw new Exception ("Probe () has no frame in the trace");

		return probe_runs_inside_kernel;
	}

	/* The same reading for Tiny (), whose throwing argument is a negative one. */
	static bool ObserveTiny ()
	{
		saw_probe = probe_runs_inside_kernel = false;
		Tiny (-1);

		if (!saw_probe)
			throw new Exception ("Probe () has no frame in the trace");

		return probe_runs_inside_kernel;
	}

	/*
	 * The same reading for Unwinder (). The catch is here rather than in the
	 * kernel, because a clause in the kernel is what this case has to be without.
	 */
	static bool ObserveUnwinder ()
	{
		saw_probe = probe_runs_inside_kernel = false;

		try {
			Unwinder (unwind_turns);
		} catch (InvalidOperationException e) {
			saw_probe = true;
			probe_runs_inside_kernel = RunsInsideKernel (e, "Unwinder");
		}

		if (!saw_probe)
			throw new Exception ("Probe () has no frame in the trace");

		return probe_runs_inside_kernel;
	}

	/* The same reading for UnwinderLoop (), and for the same reason. */
	static bool ObserveUnwinderLoop ()
	{
		saw_probe = probe_runs_inside_kernel = false;

		try {
			UnwinderLoop (unwind_loop_turns);
		} catch (InvalidOperationException e) {
			saw_probe = true;
			probe_runs_inside_kernel = RunsInsideKernel (e, "UnwinderLoop");
		}

		if (!saw_probe)
			throw new Exception ("Probe () has no frame in the trace");

		return probe_runs_inside_kernel;
	}

	static int fails;

	static void Check (bool ok, string what)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	/// Puts one kernel at tier 1, then runs it and answers whether it folded.
	static bool Run (string name, bool is_heavy, int n)
	{
		MethodInfo kernel = typeof (Program).GetMethod (name,
			BindingFlags.Static | BindingFlags.NonPublic);

		/*
		 * Tier 1 first, and asked for rather than waited for: an interpreted
		 * caller reaches an interpreted callee without the runtime being asked
		 * for it, so the calls below leave the kernel where it started. The
		 * counter is in the tier-1 body and counts nothing until there is one.
		 */
		if (!Mono.Tiering.MonoTier.PromoteNow (kernel.MethodHandle.Value, tier1)) {
			Console.WriteLine ("FAIL: {0} () would not compile at tier 1", name);
			++fails;
			return false;
		}

		Check (!Observe (is_heavy), "the helper has a body of its own before tier 2");

		int want = is_heavy ? HeavyKernel (n, false) : LightKernel (n, false);

		for (int i = 0; i < calls; ++i)
			Check ((is_heavy ? HeavyKernel (n, false) : LightKernel (n, false)) == want,
			       "the answer stays the same");

		/*
		 * The promotion is queued, so the tier-2 body lands on a compile worker
		 * rather than on this thread. Give it a bounded wait. Each try is another
		 * call of the kernel, which keeps the calls it charges far below the
		 * threshold in both arms.
		 */
		bool folded = false;

		for (int i = 0; i < 100 && !folded; ++i) {
			folded = Observe (is_heavy);
			if (!folded)
				Thread.Sleep (10);
		}

		Check ((is_heavy ? HeavyKernel (n, false) : LightKernel (n, false)) == want,
		       "the answer at the end is the answer at the start");

		return folded;
	}

	/// Puts Tiny () at tier 1, spends its counter in calls, and answers whether it
	/// folded.
	static bool RunTiny ()
	{
		MethodInfo tiny = typeof (Program).GetMethod ("Tiny",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!Mono.Tiering.MonoTier.PromoteNow (tiny.MethodHandle.Value, tier1)) {
			Console.WriteLine ("FAIL: Tiny () would not compile at tier 1");
			++fails;
			return false;
		}

		Check (!ObserveTiny (), "the helper has a body of its own before tier 2");

		for (int i = 0; i < tiny_calls; ++i)
			Check (Tiny (i) == i * 3 + 1, "the answer stays the same");

		bool folded = false;

		for (int i = 0; i < 100 && !folded; ++i) {
			folded = ObserveTiny ();
			if (!folded)
				Thread.Sleep (10);
		}

		return folded;
	}

	/// Puts Unwinder () at tier 1, spends its counter in calls, and answers whether
	/// it folded.
	static bool RunUnwinder ()
	{
		MethodInfo unwinder = typeof (Program).GetMethod ("Unwinder",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!Mono.Tiering.MonoTier.PromoteNow (unwinder.MethodHandle.Value, tier1)) {
			Console.WriteLine ("FAIL: Unwinder () would not compile at tier 1");
			++fails;
			return false;
		}

		Check (!ObserveUnwinder (), "the helper has a body of its own before tier 2");

		bool came_back = false;

		for (int i = 0; i < unwind_calls; ++i) {
			try {
				Unwinder (unwind_turns);
				came_back = true;
			} catch (InvalidOperationException) {
			}
		}

		Check (!came_back, "the kernel leaves only through the exception");

		bool folded = false;

		for (int i = 0; i < 100 && !folded; ++i) {
			folded = ObserveUnwinder ();
			if (!folded)
				Thread.Sleep (10);
		}

		return folded;
	}

	/// Puts UnwinderLoop () at tier 1, spends its counter in loop turns, and
	/// answers whether it folded.
	static bool RunUnwinderLoop ()
	{
		MethodInfo unwinder = typeof (Program).GetMethod ("UnwinderLoop",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!Mono.Tiering.MonoTier.PromoteNow (unwinder.MethodHandle.Value, tier1)) {
			Console.WriteLine ("FAIL: UnwinderLoop () would not compile at tier 1");
			++fails;
			return false;
		}

		Check (!ObserveUnwinderLoop (), "the helper has a body of its own before tier 2");

		bool came_back = false;

		for (int i = 0; i < unwind_loop_calls; ++i) {
			try {
				UnwinderLoop (unwind_loop_turns);
				came_back = true;
			} catch (InvalidOperationException) {
			}
		}

		Check (!came_back, "the kernel leaves only through the exception");

		bool folded = false;

		for (int i = 0; i < 100 && !folded; ++i) {
			folded = ObserveUnwinderLoop ();
			if (!folded)
				Thread.Sleep (10);
		}

		return folded;
	}

	public static int Main ()
	{
		string threshold = Environment.GetEnvironmentVariable (
			"MONO_LLVM_JIT_TIER2_THRESHOLD");
		bool want_tier2 = threshold != "0";

		bool heavy_folded = Run ("HeavyKernel", true, heavy);
		bool light_folded = Run ("LightKernel", false, light);
		bool tiny_folded = RunTiny ();
		bool unwinder_folded = RunUnwinder ();
		bool unwinder_loop_folded = RunUnwinderLoop ();

		if (want_tier2) {
			Check (heavy_folded, "the work a body does takes it to tier 2");
			// The other half of the counter, and the half a threshold on work
			// alone never reaches. Tiny () does almost nothing in a call and
			// promotes on the number of them.
			Check (tiny_folded, "the calls a body takes take it to tier 2");
			// A body that leaves only through a callee's exception. What its
			// entry charges is enough here, and the calls are what spend it.
			Check (unwinder_folded, "a body that always unwinds still reaches tier 2");
			// The same shape with too few calls to promote on the entry. Only
			// the pad the counter's fault clause names as its handler charges
			// the turns of the loop, so this is what asserts the pad runs.
			Check (unwinder_loop_folded,
			       "an unwinding body is charged the turns its loop made");
		} else {
			Check (!heavy_folded, "and no counter promotes a body while the threshold is zero");
			Check (!tiny_folded, "and neither does a body that only takes calls");
			Check (!unwinder_folded, "and neither does a body that always unwinds");
			Check (!unwinder_loop_folded, "and neither does one whose loop is what it spends");
		}

		/*
		 * The control, and it holds in both arms. LightKernel () has the body
		 * HeavyKernel () has and the calls Tiny () does not: sixty-four of them,
		 * which is three hundred thousand against a threshold of ten million. So
		 * it reaches the threshold on neither half and stays where it is.
		 */
		Check (!light_folded, "a body with too little of either stays at tier 1");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
