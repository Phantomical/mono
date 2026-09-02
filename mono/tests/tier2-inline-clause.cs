using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Whether the tier-2 cost model folds a clause-bearing callee. The suite
 * runs this file twice, and MONO_FOLD_CLAUSES tells the test which arm it
 * is in.
 *
 * DiesOnFold ()'s clause is dead once folded: Root () only ever passes a
 * freshly allocated Foo, so f != null answers true off the allocation's own
 * nonnull return, with no cast or class guess involved. StaysLive ()'s clause
 * stays live: Root () forwards its own takeClause parameter, which the fold
 * cannot see a fixed value for, so the clause runs on some calls and not
 * others. A finally clause that stays live is not the dead weight
 * DiesOnFold ()'s is, but it still folds - eh-gather.cpp reads such a
 * clause's owner straight off its own marker, so the fold merges the clause
 * into Root ()'s own table instead of needing it gone.
 *
 * NoLandingPad ()'s clause has no landing pad at all: its try region calls
 * nothing, so the front end never builds one. Its finally body still runs on
 * every call, so the clause stays live the same way StaysLive ()'s does. The
 * fold has to catch this one through the marker channel alone, and it folds
 * for the same reason StaysLive ()'s does.
 *
 * StaysLiveCatch ()'s clause is a genuine catch, live the same way
 * StaysLive ()'s finally is - Root () forwards the same kind of parameter the
 * fold cannot see a fixed value for. It folds for the same reason StaysLive
 * ()'s finally does: mergeable_clause_kinds_only () (passes/top-down-inline.cpp)
 * accepts a catch the same as a finally or a fault.
 *
 * StaysLiveSiblingCatch ()'s try is protected by two catches rather than one -
 * the same shared pad and shared PC range mono_lsda.cpp's
 * ranges_equal_or_disjoint () already accepts for the root's own sibling
 * catches. Folding splices both into the root's table, so this is what
 * exercises that join once the clauses are not the root's own.
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

	public static int StaysLiveCatch (Foo f, bool takeClause, bool throwing)
	{
		if (!takeClause) {
			int v = Fast (f);

			if (throwing)
				throw new InvalidOperationException ("stays-catch");

			return v;
		}

		try {
			return Slow (f);
		} catch (ArgumentException) {
			// Never taken - Slow () never throws one. What keeps the clause
			// live is the same as StaysLive ()'s finally: a protected call
			// the fold cannot prove will not throw, not whether the handler
			// body itself runs.
			return -1;
		}
	}

	public static int StaysLiveSiblingCatch (Foo f, bool takeClause, bool throwing)
	{
		if (!takeClause) {
			int v = Fast (f);

			if (throwing)
				throw new InvalidOperationException ("stays-sibling-catch");

			return v;
		}

		try {
			return Slow (f);
		} catch (ArgumentException) {
			// Neither sibling is ever taken, the same as StaysLiveCatch ()'s -
			// both stay live for the same reason its one clause does.
			return -1;
		} catch (ArithmeticException) {
			return -2;
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
	static bool saw_dies, saw_stays, saw_catch, saw_sibling, saw_none;
	static bool folded_dies, folded_stays, folded_catch, folded_sibling, folded_none;

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
		} else if (helper == "StaysLiveCatch") {
			saw_catch |= trace.Contains ("Clauses.StaysLiveCatch");
			folded_catch |= RunsInsideRoot (e, "StaysLiveCatch");
		} else if (helper == "StaysLiveSiblingCatch") {
			saw_sibling |= trace.Contains ("Clauses.StaysLiveSiblingCatch");
			folded_sibling |= RunsInsideRoot (e, "StaysLiveSiblingCatch");
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
			total += Clauses.StaysLiveCatch (f, takeClause, throwing);
		} catch (InvalidOperationException e) {
			Record (e, "StaysLiveCatch");
		}

		try {
			total += Clauses.StaysLiveSiblingCatch (f, takeClause, throwing);
		} catch (InvalidOperationException e) {
			Record (e, "StaysLiveSiblingCatch");
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
		bool folding = Environment.GetEnvironmentVariable ("MONO_FOLD_CLAUSES") != "off";
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (false, true);

		Check (saw_dies && saw_stays && saw_catch && saw_sibling && saw_none,
			"every helper has a frame before tier 2");
		Check (!folded_dies && !folded_stays && !folded_catch && !folded_sibling && !folded_none,
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

		saw_dies = saw_stays = saw_catch = saw_sibling = saw_none = false;
		folded_dies = folded_stays = folded_catch = folded_sibling = folded_none = false;

		Check (want == Root (false, true), "the answer at tier 2 is the answer before it");
		Check (saw_dies && saw_stays && saw_catch && saw_sibling && saw_none,
			"every helper still has a frame at tier 2");

		if (folding) {
			Check (folded_dies, "a clause the fold makes dead folds into the root");
			Check (folded_stays, "a live finally clause now folds into the root too");
			Check (folded_catch, "a live catch clause folds into the root too");
			Check (folded_sibling, "a live pair of sibling catches folds into the root too");
			Check (folded_none,
				"a live finally clause with no landing pad folds the same way");
		} else {
			Check (!folded_dies,
				"MONO_FOLD_CLAUSES=off refuses it the way the pre-pass does");
			Check (!folded_stays, "MONO_FOLD_CLAUSES=off refuses a live finally too");
			Check (!folded_catch, "MONO_FOLD_CLAUSES=off refuses a live catch too");
			Check (!folded_sibling, "MONO_FOLD_CLAUSES=off refuses sibling catches too");
			Check (!folded_none,
				"MONO_FOLD_CLAUSES=off refuses a landing-pad-free finally too");
		}

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
