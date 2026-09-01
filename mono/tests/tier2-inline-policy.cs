using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * The call-site bonuses passes/inline-policy.cpp adds to the tier-2 cost model.
 * Each shape below is a body the model declines on LLVM's own numbers and takes
 * once mono says what the fold is worth, so the suite runs twice: once on the
 * defaults and once with every bonus set to zero, and reads MONO_INLINE_POLICY
 * to know which arm it is in.
 *
 * The trivial pre-pass is off in both arms
 * (--llvm-opt=-mono-inline-il-limit=0). Make () is one allocation that is
 * returned, which is a shape that pre-pass
 * folds on sight, and a fold it takes says nothing about the cost model.
 *
 * What says a fold happened is the stack trace, the way tier2-inline-cost.cs
 * reads it: a folded body owns no code, so its frame reports the offset into
 * Root () that it was folded at, and a body that was really called reports an
 * offset into itself.
 *
 * Both suites name a cold-callsite threshold of their own, because the bodies
 * have to sit between what the model gives them and what the bonus adds. The
 * off arm costs each of them 180. The on arm prices Measure's interface
 * dispatch as the load it lowers to and gives 135, and leaves Make where it is,
 * so one threshold has to serve two costs.
 *
 * Measure is the tighter of the two: it has to decline at 180 and fold at
 * 135 with the argument bonus of 50, which puts the threshold between 85 and
 * 135. 110 takes the middle and leaves 25 either way. Make then declines at 180
 * and folds with the return bonus of 100. Re-measure all of it when this starts
 * failing on one arm:
 *
 *   MONO_LLVM_JIT_TRACE=1 MONO_INLINE_POLICY=off mono-sgen \
 *     --llvm-opt=-mono-tier2-threshold=0 \
 *     --llvm-opt=-mono-inline-il-limit=0 \
 *     --llvm-opt=-mono-inline-cost-full \
 *     --llvm-opt=-mono-inline-devirt-return-bonus=0 \
 *     --llvm-opt=-mono-inline-devirt-arg-bonus=0 tier2-inline-policy.exe
 *
 * That gives the on arm's costs, because zeroing the bonuses leaves the answers
 * the model makes for itself. The off arm's costs want the rest of the options
 * runtime-suites.cmake names for it, which turn those answers off as well.
 *
 * `-mono-inline-cost-full` is what makes the printed cost the whole cost. The
 * model stops adding once it is past the budget, so the cost a plain run prints
 * is cut off there and says nothing about how far past it went.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

interface IShape {
	int Area ();
}

class Box : IShape {
	public int w, h;

	public Box (int a, int b) { w = a; h = b; }

	public int Area () { return w * h + 1; }
}

static class Shapes {
	/*
	 * Allocates under a class it names and answers with the interface, which
	 * the caller then dispatches on. Folding it puts the vtable where the
	 * caller's dispatch reads a pointer, so fold_dispatch_sites () can read it.
	 */
	public static IShape Make (int w, int h, bool yes)
	{
		Box made = new Box (w + 1, h + 1);

		if (yes)
			throw new InvalidOperationException ("make");

		return made;
	}

	/*
	 * Dispatches on a parameter the site passes a fresh Box in. The class
	 * travels in with the argument, so the two calls below resolve once the
	 * body sits beside the allocation.
	 */
	public static int Measure (IShape s, bool yes)
	{
		int total = s.Area () + s.Area () * 3;

		if (yes)
			throw new InvalidOperationException ("measure");

		return total;
	}
}

static class Program {
	/* Which helper the trace taken inside Root () named. */
	static bool saw_make, saw_measure;

	/* Which of them ran inside Root ()'s code rather than in a body of its own. */
	static bool folded_make, folded_measure;

	/// Whether the helper's frame covers the same code as Root ()'s.
	static bool RunsInsideRoot (Exception e, string helper)
	{
		StackTrace st = new StackTrace (e, false);
		int in_helper = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Shapes" && m.Name == helper)
				in_helper = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_helper >= 0 && in_helper == in_root;
	}

	static void Record (Exception e)
	{
		string trace = e.StackTrace ?? "";

		saw_make |= trace.Contains ("Shapes.Make");
		saw_measure |= trace.Contains ("Shapes.Measure");

		folded_make |= RunsInsideRoot (e, "Make");
		folded_measure |= RunsInsideRoot (e, "Measure");

		if (!trace.Contains ("Program.Root"))
			throw new Exception ("the frame that caught it is missing: " + trace);
	}

	/*
	 * The warm-up calls all take the early return, so the profile reads the
	 * block below as cold and the model weighs its sites against
	 * ColdCallSiteThreshold. That is the range a bonus decides anything in: a
	 * hot site is weighed against HotCallSiteThreshold, which is large enough
	 * to swallow every body this file holds.
	 */
	static int Root (int n, bool throwing)
	{
		int total = n * 2;

		if (n >= 0)
			return total;

		try {
			// The dispatch on the answer is what the return bonus reads.
			total += Make (n, throwing).Area ();
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			Record (e);
		}

		try {
			// A fresh allocation in the argument is what the argument bonus reads.
			total += Shapes.Measure (new Box (n & 3, 2), throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			Record (e);
		}

		return total;
	}

	static IShape Make (int n, bool throwing)
	{
		return Shapes.Make (n & 7, 3, throwing);
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
		bool bonuses = Environment.GetEnvironmentVariable ("MONO_INLINE_POLICY") != "off";
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		// Tier 1 first and asked for, for the reason tier2-inline-cost.cs gives.
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (-2, true);

		Check (saw_make && saw_measure, "every helper has a frame before tier 2");
		Check (!folded_make && !folded_measure,
			"and every one of them runs in a body of its own before tier 2");

		// Enough calls to leave counts on the tier-1 body.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_make = saw_measure = false;
		folded_make = folded_measure = false;

		Check (want == Root (-2, true), "the answer at tier 2 is the answer before it");
		Check (saw_make && saw_measure, "every helper still has a frame at tier 2");

		if (bonuses) {
			Check (folded_make,
				"the return bonus folds a body that answers with what it allocated");
			Check (folded_measure,
				"the argument bonus folds a body that dispatches on a fresh argument");
		} else {
			Check (!folded_make,
				"the return bonus is what folds the body that allocates its answer");
			Check (!folded_measure,
				"the argument bonus is what folds the body that dispatches");
		}

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
