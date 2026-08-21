using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * The callees a compile folds into their caller before any cost model looks at
 * them. Both compiled tiers do it, so both are checked.
 * Mono.Tiering.MonoTier::PromoteNow compiles Root () at the tier it is given, on
 * this thread and whatever tier the method was running at, so the test needs no
 * environment and races no compile worker.
 *
 * Every shape is checked at each tier, and the answers all have to agree with
 * the first one. What says the fold really happened is the stack trace: a folded
 * body has no frame of its own, so the helper that threw is missing from the
 * trace, and the helpers the gates refuse are still in it.
 * MONO_LLVM_JIT_INLINE_IL_LIMIT=0 turns the pre-pass off, and this test then
 * fails on those checks alone.
 *
 * The first call claims nothing about frames. Which engine it runs in is the
 * arm's choice: interpreted where tier 0 is on, compiled where it is off.
 *
 * Tier 2 has a second inliner behind this one, which weighs what the shape test
 * declines - tier2-inline-cost.cs is that one's test. Here it only matters for
 * FailBranch (), which the shape test declines and the cost model then takes.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Inner {
	public int w = 41;
}

class Middle {
	public Inner z = new Inner ();
}

class Outer {
	public Middle y = new Middle ();
}

class Held {
	public int v;
	public Held (int v) { this.v = v; }
}

interface IThing {
	int Value ();
}

class Thing : IThing {
	int v;
	public Thing (int v) { this.v = v; }
	public int Value () { return v; }
}

static class Trivial {
	public static int Constant () { return 7; }

	public static bool True () { return true; }

	public static int Chain (Outer o) { return o.y.z.w; }

	public static void Store (Outer o, int w) { o.y.z.w = w; }

	// A forwarder whose target is a candidate as well, so the pre-pass has to
	// reach through the body it just added.
	public static int Nested (Outer o) { return Deeper (o.y); }

	public static int Deeper (Middle m) { return m.z.w; }

	public static int Forward (Outer o, int n) { return Add (o.y.z.w, n); }

	// Not a shape the pre-pass takes: the add is a computation of its own.
	public static int Add (int a, int b) { return a + b; }

	public static Held Make (int v) { return new Held (v); }

	// The object leaves as an interface, which is the return type and not
	// anything the IL does.
	public static IThing MakeThing (int v) { return new Thing (v); }

	public static void Fail (string what) { throw new InvalidOperationException (what); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void FailNoInline (string what)
	{
		throw new InvalidOperationException (what);
	}

	// A branch, so the shape test declines it however small it is - and the cost
	// model behind it then weighs it and takes it.
	public static void FailBranch (string what, bool yes)
	{
		if (yes)
			throw new InvalidOperationException (what);
	}
}

static class Program {
	static Outer o = new Outer ();

	/* Which of the three helpers the trace taken inside Root () named. */
	static bool saw_fail, saw_no_inline, saw_branch;

	static void Record (Exception e)
	{
		string trace = e.StackTrace ?? "";

		saw_fail |= trace.Contains ("Trivial.Fail (");
		saw_no_inline |= trace.Contains ("Trivial.FailNoInline");
		saw_branch |= trace.Contains ("Trivial.FailBranch");

		if (!trace.Contains ("Program.Root"))
			throw new Exception ("the frame that caught it is missing: " + trace);
	}

	/*
	 * Every shape is called from here rather than from a helper of its own,
	 * because the pre-pass only looks at the method being compiled: a shape
	 * called from somewhere else is folded into that method, at whatever tier
	 * that method reaches.
	 */
	static int Root (int n)
	{
		int total = Trivial.Constant () + Trivial.Chain (o) + Trivial.Nested (o);

		total += Trivial.Forward (o, n);
		total += Trivial.Make (n).v;
		total += Trivial.MakeThing (n).Value ();

		// Put the field back where the reads above expect it, so calling Root ()
		// twice gives the same number both times.
		Trivial.Store (o, 40);
		total += Trivial.Chain (o);
		Trivial.Store (o, 41);

		if (!Trivial.True ())
			return -1;

		try {
			Trivial.Fail ("boom");
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			Record (e);
		}

		try {
			Trivial.FailNoInline ("bang");
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			Record (e);
		}

		try {
			Trivial.FailBranch ("crash", n > 0);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			Record (e);
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

	/* MonoTier::tier1 and MonoTier::tier2, as PromoteNow takes them. */
	const int tier1 = 2;
	const int tier2 = 3;

	static bool AtTier (MethodInfo root, int tier, string name, int want)
	{
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, tier)) {
			Console.WriteLine ("FAIL: Root () would not compile at {0}", name);
			return false;
		}

		saw_fail = saw_no_inline = saw_branch = false;

		int got = Root (3);

		Check (want == got, "the answer at " + name);

		/*
		 * A folded body's code belongs to the frame it was folded into, so the
		 * trace names that frame and not the helper. Extending the side tables
		 * to carry inlined frames is what would put Trivial.Fail back, and this
		 * is where to say so.
		 */
		Check (!saw_fail, "the folded helper has no frame of its own at " + name);
		Check (saw_no_inline, "NoInlining keeps the helper's frame at " + name);

		/*
		 * FailBranch () is the one shape the two tiers answer differently. The
		 * shape test declines a branch at either tier. Only tier 2 has the cost
		 * model behind it that then takes the body anyway.
		 */
		if (tier == tier2)
			Check (!saw_branch, "the cost model takes what the shape test declined");
		else
			Check (saw_branch, "a helper with a branch keeps its frame at " + name);

		return true;
	}

	public static int Main ()
	{
		int want = Root (3);

		Check (want == 7 + 41 + 41 + 44 + 3 + 3 + 40 + 4 + 4 + 5,
			"the answer before any promotion");

		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!AtTier (root, tier1, "tier 1", want)
		    || !AtTier (root, tier2, "tier 2", want))
			return 1;

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
