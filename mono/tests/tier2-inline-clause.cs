using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Whether the tier-2 cost model folds a clause-bearing callee, gated by
 * MONO_LLVM_JIT_FOLD_CLAUSES.
 *
 * DiesOnFold ()'s clause is dead once folded: Root () only ever passes a
 * freshly allocated Foo, so f != null answers true off the allocation's own
 * nonnull return, with no cast or class guess involved. StaysLive ()'s clause
 * stays live: Root () forwards its own takeClause parameter, which the fold
 * cannot see a fixed value for, so the clause runs on some calls and not
 * others.
 *
 * NoLandingPad ()'s clause has no landing pad at all: its try region calls
 * nothing, so the front end never builds one. Its finally body still runs on
 * every call, so the clause stays live the same way StaysLive ()'s does. The
 * fold has to catch this one through the marker channel alone.
 *
 * What says a fold happened is the stack trace, the way tier2-inline-policy.cs
 * reads it: a folded body owns no code, so its frame reports the offset into
 * Root () that it was folded at, and a body that was really called reports an
 * offset into itself.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Foo { }

static class Clauses {
	public static int cleanups;
	public static int landingless_cleanups;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Fast (Foo f) { return 1; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Slow (Foo f) { return 2; }

	public static int DiesOnFold (Foo f, bool throwing)
	{
		if (f != null) {
			int v = Fast (f);

			if (throwing)
				throw new InvalidOperationException ("dies");

			return v;
		}

		try {
			return Slow (f);
		} finally {
			cleanups++;
		}
	}

	public static int StaysLive (Foo f, bool takeClause, bool throwing)
	{
		if (!takeClause) {
			int v = Fast (f);

			if (throwing)
				throw new InvalidOperationException ("stays");

			return v;
		}

		try {
			return Slow (f);
		} finally {
			cleanups++;
		}
	}

	public static int NoLandingPad (int x, bool throwing)
	{
		int v;

		// The try body has no call that can unwind, so the front end
		// builds no landing pad for this clause.
		try {
			v = x + 1;
		} finally {
			landingless_cleanups++;
		}

		if (throwing)
			throw new InvalidOperationException ("no-landing-pad");

		return v;
	}
}

static class Program {
	static bool saw_dies, saw_stays, saw_none;
	static bool folded_dies, folded_stays, folded_none;

	static bool RunsInsideRoot (Exception e, string helper)
	{
		StackTrace st = new StackTrace (e, false);
		int in_helper = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Clauses" && m.Name == helper)
				in_helper = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_helper >= 0 && in_helper == in_root;
	}

	static void Record (Exception e, string helper)
	{
		string trace = e.StackTrace ?? "";

		if (helper == "DiesOnFold") {
			saw_dies |= trace.Contains ("Clauses.DiesOnFold");
			folded_dies |= RunsInsideRoot (e, "DiesOnFold");
		} else if (helper == "StaysLive") {
			saw_stays |= trace.Contains ("Clauses.StaysLive");
			folded_stays |= RunsInsideRoot (e, "StaysLive");
		} else {
			saw_none |= trace.Contains ("Clauses.NoLandingPad");
			folded_none |= RunsInsideRoot (e, "NoLandingPad");
		}

		if (!trace.Contains ("Program.Root"))
			throw new Exception ("the frame that caught it is missing: " + trace);
	}

	static int Root (bool takeClause, bool throwing)
	{
		int total = 0;
		Foo f = new Foo ();

		try {
			total += Clauses.DiesOnFold (f, throwing);
		} catch (InvalidOperationException e) {
			Record (e, "DiesOnFold");
		}

		try {
			total += Clauses.StaysLive (f, takeClause, throwing);
		} catch (InvalidOperationException e) {
			Record (e, "StaysLive");
		}

		try {
			total += Clauses.NoLandingPad (total, throwing);
		} catch (InvalidOperationException e) {
			Record (e, "NoLandingPad");
		}

		return total;
	}

	static int fails;

	static void Check (bool ok, string what)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	public static int Main ()
	{
		bool folding = Environment.GetEnvironmentVariable ("MONO_LLVM_JIT_FOLD_CLAUSES") != "0";
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (false, true);

		Check (saw_dies && saw_stays && saw_none, "every helper has a frame before tier 2");
		Check (!folded_dies && !folded_stays && !folded_none,
			"and none of them runs in a body of its own before tier 2");
		Check (Clauses.cleanups == 0, "the live clause has not run yet");
		Check (Clauses.landingless_cleanups == 1,
			"the landing-pad-free clause already ran its finally once");

		// Every call takes the fast path, so the counts tier 2 promotes on
		// carry no exception cost and StaysLive () keeps seeing both values.
		for (int i = 0; i < 20000; ++i)
			Root (i % 2 == 0, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_dies = saw_stays = saw_none = false;
		folded_dies = folded_stays = folded_none = false;

		Check (want == Root (false, true), "the answer at tier 2 is the answer before it");
		Check (saw_dies && saw_stays && saw_none, "every helper still has a frame at tier 2");

		if (folding)
			Check (folded_dies, "a clause the fold makes dead folds into the root");
		else
			Check (!folded_dies,
				"MONO_LLVM_JIT_FOLD_CLAUSES=0 refuses it the way the pre-pass does");

		Check (!folded_stays, "a clause the caller can still take never folds");

		// Live on every call, so this stays refused whether
		// MONO_LLVM_JIT_FOLD_CLAUSES is on or off, unlike DiesOnFold ()'s
		// check above.
		Check (!folded_none, "a clause with no landing pad at all still never folds");

		int before = Clauses.cleanups;

		Root (true, false);
		Check (Clauses.cleanups == before + 1, "the live clause still runs its finally");

		int before_landingless = Clauses.landingless_cleanups;

		Root (true, false);
		Check (Clauses.landingless_cleanups == before_landingless + 1,
			"the landing-pad-free clause still runs its finally");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
