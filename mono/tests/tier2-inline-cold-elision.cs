using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * carries_an_elision_candidate () (passes/inline-policy.cpp) lets a cold site
 * translate its candidate under the ordinary IL limit instead of the cold
 * one, when the caller's own IR already shows the argument is a fresh
 * allocation still flowing toward erasure -- the IL gate bounds cost, and
 * should not be the thing deciding an elision fold a cost model, weighing an
 * elision bonus worth far more than the cold budget, would otherwise take.
 *
 * The candidate is an instance method rather than a constructor: every
 * constructor calls its base class's own, and PointerMayBeCaptured () reads
 * that call as a potential capture of `this` unless the base ctor is marked
 * nocapture, which object::.ctor () is not here -- so a constructor callee
 * never clears call_site_bonus ()'s own capture gate, whatever this gate
 * does. Process () has no such call, so it is the shape the elision bonus
 * can actually answer for.
 *
 * Kept.Process () and Escaped.Process () are both ~113 IL bytes -- past the
 * flat 64-byte cold default, comfortably under the 256-byte ordinary one --
 * built to the same shape tier2-inline-byte-budget.cs measures at that size.
 * Each is called on a freshly constructed receiver at a cold site (never
 * taken during warm-up, so the site reads cold once Root () is promoted).
 * Kept's receiver has no other use for the caller-side scan to find --
 * carries_an_elision_candidate () answers true, and the fold should clear
 * the cold budget on the bonus. Escaped's receiver is passed to a
 * NoInlining sink first, which call_wont_fold () marks a way out for the
 * pointer -- the scan answers escapes, the check answers false, and Escaped
 * should still be refused at the flat 64-byte limit, same as before this
 * landed.
 *
 * What says a fold happened is the stack trace, the way tier2-inline-cost.cs
 * reads it: a folded body owns no code, so its frame reports the offset into
 * Root () it was folded at, and a body that was really called reports an
 * offset into itself.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Kept {
	int a, b, c, d;

	public void Process (int n, bool yes)
	{
		int a = n + 1, b = n + 2, c = n + 3, d = n + 4;

		a = a * b + c - d;
		b = b * c + d - a;
		c = c * d + a - b;
		d = d * a + b - c;

		this.a = a; this.b = b; this.c = c; this.d = d;

		if (yes)
			throw new InvalidOperationException ("kept");
	}
}

class Escaped {
	int a, b, c, d;

	public void Process (int n, bool yes)
	{
		int a = n + 1, b = n + 2, c = n + 3, d = n + 4;

		a = a * b + c - d;
		b = b * c + d - a;
		c = c * d + a - b;
		d = d * a + b - c;

		this.a = a; this.b = b; this.c = c; this.d = d;

		if (yes)
			throw new InvalidOperationException ("escaped");
	}
}

static class Sink {
	// NoInlining is what call_wont_fold () reads to call a pass-through a way
	// out for the pointer -- see carries_an_elision_candidate ()'s own
	// comment. The body does nothing; only the mark matters.
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void Consume (object o)
	{
	}
}

static class Program {
	static bool sawKept, sawEscaped, foldedKept, foldedEscaped;

	static bool RunsInsideRoot (Exception e, string type, string helper)
	{
		StackTrace st = new StackTrace (e, false);
		int in_helper = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == type && m.Name == helper)
				in_helper = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_helper >= 0 && in_helper == in_root;
	}

	static int Root (int n, bool takeRare)
	{
		int total = 0;

		if (takeRare) {
			try {
				Kept kept = new Kept ();
				kept.Process (n, true);
			} catch (InvalidOperationException e) {
				sawKept = true;
				foldedKept = RunsInsideRoot (e, "Kept", "Process");
			}

			try {
				Escaped escaped = new Escaped ();
				Sink.Consume (escaped);
				escaped.Process (n, true);
			} catch (InvalidOperationException e) {
				sawEscaped = true;
				foldedEscaped = RunsInsideRoot (e, "Escaped", "Process");
			}
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
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		// Never taken, so both sites read cold once Root () is promoted.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		sawKept = sawEscaped = foldedKept = foldedEscaped = false;

		Root (4, true);

		Check (sawKept && sawEscaped, "both Process () calls threw at tier 2");
		Check (foldedKept,
			"a cold site whose fresh receiver stays unescaped folds under "
			+ "the ordinary limit");
		Check (!foldedEscaped,
			"a cold site whose receiver escapes to a NoInlining sink keeps "
			+ "the flat cold limit");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
