using System;
using System.Diagnostics;
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
 * the first one. What says the fold really happened is the stack trace. Every
 * helper that threw has a frame in it either way, but a folded body owns no
 * code: its frame reports the offset into the caller it was folded at, and a
 * helper the gates refuse reports an offset into itself.
 * --llvm-opt=-mono-inline-il-limit=0 turns the pre-pass off, and this test
 * then fails on those checks alone.
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

	/*
	 * A forwarder onto a method that forwards to an icall, which is two links of
	 * chain for one fold: Array:Clone () calls object:MemberwiseClone (). A null
	 * array raises where the call stands.
	 */
	public static object CloneOf (Array a) { return a.Clone (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void FailNoInline (string what)
	{
		throw new InvalidOperationException (what);
	}

	/*
	 * A forwarder onto FailNoInline (). The mark keeps that method out of every
	 * fold, so it holds a body and a frame of its own whatever its caller does.
	 * The mark says nothing about this forwarder, so this one folds.
	 */
	public static void FailThroughNoInline (string what) { FailNoInline (what); }

	// A branch, so the shape test declines it however small it is - and the cost
	// model behind it then weighs it and takes it.
	public static void FailBranch (string what, bool yes)
	{
		if (yes)
			throw new InvalidOperationException (what);
	}

	/*
	 * Asks who called it and throws the answer, so one trace says both what
	 * GetCurrentMethod () named and where the body that asked ran. Three calls,
	 * so the shape test declines it and only the cost model takes it - which
	 * makes this the tier-2 arm for a folded body that walks the stack.
	 *
	 * The answer has to stay FailCurrent at both tiers. It comes off the frames
	 * the compile recorded, and a walk blind to those names the root instead.
	 */
	public static void FailCurrent ()
	{
		throw new InvalidOperationException (MethodBase.GetCurrentMethod ().Name);
	}
}

static class Program {
	static Outer o = new Outer ();

	/* Which helpers the traces taken inside the roots below named. */
	static bool saw_fail, saw_no_inline, saw_through, saw_branch, saw_clone;

	/* Which of them ran inside their root's code rather than in a body of its own. */
	static bool folded_fail, folded_no_inline, folded_through, folded_branch,
		folded_clone, folded_current;

	/* What GetCurrentMethod () named inside FailCurrent (). */
	static string current_name;

	/*
	 * Whether the helper's frame covers the same code as root's.
	 *
	 * A folded body has no code of its own, so the frame reported for it names
	 * the call site in root that it was folded at - the same native offset
	 * root's own frame reports. A helper that was really called runs in its
	 * own body and reports an offset into that.
	 */
	static bool RunsInside (Exception e, string helper, string root)
	{
		StackTrace st = new StackTrace (e, false);
		int in_helper = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Trivial" && m.Name == helper)
				in_helper = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == root)
				in_root = f.GetNativeOffset ();
		}

		return in_helper >= 0 && in_helper == in_root;
	}

	/*
	 * A root of its own, because the cost model weighs a site against the size
	 * of the method it stands in. Put this call in Root () and tier 2 stops
	 * folding FailBranch ().
	 */
	static void CloneRoot ()
	{
		try {
			Trivial.CloneOf (null);
		} catch (NullReferenceException e) {
			saw_clone |= (e.StackTrace ?? "").Contains ("Trivial.CloneOf");
			folded_clone |= RunsInside (e, "CloneOf", "CloneRoot");
		}
	}

	/* A root of its own, for the reason CloneRoot () gives. */
	static void CurrentRoot ()
	{
		try {
			Trivial.FailCurrent ();
		} catch (InvalidOperationException e) {
			current_name = e.Message;
			folded_current |= RunsInside (e, "FailCurrent", "CurrentRoot");
		}
	}

	static void Record (Exception e)
	{
		string trace = e.StackTrace ?? "";

		saw_fail |= trace.Contains ("Trivial.Fail (");
		saw_no_inline |= trace.Contains ("Trivial.FailNoInline");
		saw_through |= trace.Contains ("Trivial.FailThroughNoInline");
		saw_branch |= trace.Contains ("Trivial.FailBranch");

		folded_fail |= RunsInside (e, "Fail", "Root");
		folded_no_inline |= RunsInside (e, "FailNoInline", "Root");
		folded_through |= RunsInside (e, "FailThroughNoInline", "Root");
		folded_branch |= RunsInside (e, "FailBranch", "Root");

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
			Trivial.FailThroughNoInline ("via");
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

	static bool AtTier (MethodInfo root, MethodInfo clone_root, MethodInfo current_root,
	                    int tier, string name, int want)
	{
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, tier)) {
			Console.WriteLine ("FAIL: Root () would not compile at {0}", name);
			return false;
		}

		if (!Mono.Tiering.MonoTier.PromoteNow (clone_root.MethodHandle.Value, tier)) {
			Console.WriteLine ("FAIL: CloneRoot () would not compile at {0}", name);
			return false;
		}

		if (!Mono.Tiering.MonoTier.PromoteNow (current_root.MethodHandle.Value, tier)) {
			Console.WriteLine ("FAIL: CurrentRoot () would not compile at {0}", name);
			return false;
		}

		saw_fail = saw_no_inline = saw_through = saw_branch = saw_clone = false;
		folded_fail = folded_no_inline = folded_through = folded_branch =
			folded_clone = folded_current = false;
		current_name = null;

		int got = Root (3);

		CloneRoot ();
		CurrentRoot ();

		Check (want == got, "the answer at " + name);

		/*
		 * A folded body keeps a frame in the trace, built from the side table
		 * the compile wrote rather than from a frame on the stack. What says
		 * the fold happened is where that frame's code is.
		 */
		Check (saw_fail, "the folded helper has a frame of its own at " + name);
		Check (folded_fail, "and it runs inside Root () at " + name);
		Check (saw_no_inline, "NoInlining keeps the helper's frame at " + name);
		Check (!folded_no_inline,
		       "and a refused helper runs in a body of its own at " + name);
		Check (saw_through, "a forwarder onto it has a frame at " + name);
		Check (folded_through,
		       "and the mark on its target leaves it foldable at " + name);

		/*
		 * A gate that refuses a forwarder for what it reaches keeps CloneOf ()
		 * in a body of its own and fails this.
		 */
		Check (saw_clone, "a forwarder onto an icall has a frame at " + name);
		Check (folded_clone, "and it runs inside CloneRoot () at " + name);

		/*
		 * FailBranch () is the one shape the two tiers answer differently. The
		 * shape test declines a branch at either tier. Only tier 2 has the cost
		 * model behind it that then takes the body anyway.
		 */
		if (tier == tier2)
			Check (folded_branch, "the cost model takes what the shape test declined");
		else
			Check (saw_branch && !folded_branch,
			       "a helper with a branch keeps a body of its own at " + name);

		/*
		 * A body that asks who called it. The name has to hold at both tiers,
		 * and tier 2 is where it is asked of a body the cost model folded in.
		 */
		Check (current_name == "FailCurrent",
		       "GetCurrentMethod () names the body that asked at " + name
		       + ", not " + (current_name ?? "nothing"));

		if (tier == tier2)
			Check (folded_current,
			       "and the cost model folded that body into CurrentRoot ()");

		return true;
	}

	public static int Main ()
	{
		int want = Root (3);

		Check (want == 7 + 41 + 41 + 44 + 3 + 3 + 40 + 4 + 4 + 3 + 5,
			"the answer before any promotion");

		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);
		MethodInfo clone_root = typeof (Program).GetMethod ("CloneRoot",
			BindingFlags.Static | BindingFlags.NonPublic);
		MethodInfo current_root = typeof (Program).GetMethod ("CurrentRoot",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!AtTier (root, clone_root, current_root, tier1, "tier 1", want)
		    || !AtTier (root, clone_root, current_root, tier2, "tier 2", want))
			return 1;

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
