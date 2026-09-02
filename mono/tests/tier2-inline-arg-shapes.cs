using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * The devirt-arg bonus's own gates: `dispatches_unresolved_on ()`, which
 * decides whether the callee dispatches on the parameter at all, and the
 * per-argument loop in `call_site_bonus ()`, which decides whether the
 * caller's operand names a class. Four shapes each miss one of the two, and
 * the suite runs twice, once on the defaults and once with the bonus zeroed,
 * reading MONO_INLINE_POLICY to know which arm it is in.
 *
 * The trivial pre-pass is off in both arms (--llvm-opt=-mono-inline-il-limit=0),
 * so a fold this reads is the cost model's. What says a fold happened is the
 * stack trace, the way tier2-inline-cost.cs reads it: a folded body owns no
 * code, so its frame reports the offset into Root () that it was folded at,
 * and a body that was really called reports an offset into itself.
 *
 * `Holder` carries a finalizer so its own allocation takes
 * `mono.alloc.object.kept` rather than `mono.alloc.object` (CLAUDE.md,
 * "An allocation is one call until late as well"): without it, `h` itself
 * would independently earn the *scalarize* bonus (its only other use is a
 * read), which folds `MeasureField` on its own and leaves nothing for this
 * suite to tell apart from the devirt-arg bonus under test.
 *
 * Each of the four declines at 165, 175, 145 and 140 with every mono bonus
 * off, and the argument bonus is 50, which puts the threshold between 125 and
 * 140 for every one of them to land on the right side. 133 takes the middle.
 * Re-measure all of it when this starts failing on one arm:
 *
 *   MONO_LLVM_JIT_TRACE=1 MONO_INLINE_POLICY=off mono-sgen \
 *     --llvm-opt=-mono-tier2-threshold=0 \
 *     --llvm-opt=-mono-inline-il-limit=0 \
 *     --llvm-opt=-mono-inline-cost-full \
 *     --llvm-opt=-mono-inline-devirt-arg-bonus=0 tier2-inline-arg-shapes.exe
 *
 * `-mono-inline-cost-full` is what makes the printed cost the whole cost. The
 * model stops adding once it is past the budget, so the cost a plain run
 * prints is cut off there and says nothing about how far past it went.
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

class Holder {
	public IShape Inner;

	public Holder (IShape i) { Inner = i; }

	~Holder () { }
}

sealed class Registry {
	public static readonly Box Shared = new Box (3, 4);
}

static class Shapes {
	/* A dispatch one field away from the parameter: h.Inner.Area (), not
	 * h.Area (). */
	public static int MeasureField (Holder h, bool yes)
	{
		int total = h.Inner.Area () + h.Inner.Area () * 3;

		if (yes)
			throw new InvalidOperationException ("field");

		return total;
	}

	/* A type test on the parameter itself, rather than a dispatch on it. */
	public static int MeasureCast (object o, bool yes)
	{
		int total = ((Box) o).Area () + ((Box) o).Area () * 3;

		if (yes)
			throw new InvalidOperationException ("cast");

		return total;
	}

	/* Dispatches on the parameter, which the site fills with an initonly
	 * static's own read rather than with a fresh allocation. */
	public static int MeasureStatic (IShape s, bool yes)
	{
		int total = s.Area () + s.Area () * 3;

		if (yes)
			throw new InvalidOperationException ("static");

		return total;
	}

	/* Dispatches on the parameter, which the site fills with a PHI of two
	 * allocations under the same class. */
	public static int MeasurePhi (IShape s, bool yes)
	{
		int total = s.Area () + s.Area () * 3;

		if (yes)
			throw new InvalidOperationException ("phi");

		return total;
	}
}

static class Program {
	static bool saw_field, saw_cast, saw_static, saw_phi;
	static bool folded_field, folded_cast, folded_static, folded_phi;

	/// Whether \p helper's frame covers the same code as Root ()'s.
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

	/*
	 * The warm-up calls all take the early return, so the profile reads the
	 * block below as cold and the model weighs its sites against
	 * ColdCallSiteThreshold. That is the range a bonus decides anything in.
	 */
	static int Root (int n, bool throwing)
	{
		int total = n * 2;

		if (n >= 0)
			return total;

		try {
			total += Shapes.MeasureField (new Holder (new Box (n & 3, 2)), throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_field |= (e.StackTrace ?? "").Contains ("Shapes.MeasureField");
			folded_field |= RunsInsideRoot (e, "MeasureField");
		}

		try {
			total += Shapes.MeasureCast (new Box (n & 3, 2), throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_cast |= (e.StackTrace ?? "").Contains ("Shapes.MeasureCast");
			folded_cast |= RunsInsideRoot (e, "MeasureCast");
		}

		try {
			total += Shapes.MeasureStatic (Registry.Shared, throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_static |= (e.StackTrace ?? "").Contains ("Shapes.MeasureStatic");
			folded_static |= RunsInsideRoot (e, "MeasureStatic");
		}

		try {
			IShape s = (n < 0) ? new Box (n & 3, 2) : new Box ((n + 1) & 3, 2);
			total += Shapes.MeasurePhi (s, throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_phi |= (e.StackTrace ?? "").Contains ("Shapes.MeasurePhi");
			folded_phi |= RunsInsideRoot (e, "MeasurePhi");
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
		bool bonus = Environment.GetEnvironmentVariable ("MONO_INLINE_POLICY") != "off";
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (-2, true);

		Check (saw_field && saw_cast && saw_static && saw_phi,
			"every helper has a frame before tier 2");
		Check (!folded_field && !folded_cast && !folded_static && !folded_phi,
			"and every one of them runs in a body of its own before tier 2");

		// Enough calls to leave counts on the tier-1 body.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_field = saw_cast = saw_static = saw_phi = false;
		folded_field = folded_cast = folded_static = folded_phi = false;

		Check (want == Root (-2, true), "the answer at tier 2 is the answer before it");
		Check (saw_field && saw_cast && saw_static && saw_phi,
			"every helper still has a frame at tier 2");

		if (bonus) {
			Check (folded_field,
				"the argument bonus folds a body that dispatches one field from the argument");
			Check (folded_cast,
				"the argument bonus folds a body that tests the argument's own class");
			Check (folded_static,
				"the argument bonus folds a body that dispatches on an initonly static read");
			Check (folded_phi,
				"the argument bonus folds a body that dispatches on a merged allocation");
		} else {
			Check (!folded_field,
				"the argument bonus is what folds the body one field from the argument");
			Check (!folded_cast,
				"the argument bonus is what folds the body that tests the argument's class");
			Check (!folded_static,
				"the argument bonus is what folds the body dispatching on the static read");
			Check (!folded_phi,
				"the argument bonus is what folds the body dispatching on the merged allocation");
		}

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
