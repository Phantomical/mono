using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * What the tier-2 cost model answers about a receiver the call site allocated.
 *
 * An IR pointer carries no class, so a callee that dispatches on a parameter or
 * asks it for its type is opaque to the model. A compile states a class's
 * vtable as a constant, and the store an allocation carries is the one step
 * from the receiver to that constant. Behind it the dispatch slot and the
 * `System.Type` word both fold, and the arm the type test cannot reach stops
 * being counted.
 *
 * The suite runs twice, once on the defaults and once with every one of those
 * answers off, and reads MONO_INLINE_POLICY to know which arm it is in. The
 * trivial pre-pass is off in both (--llvm-opt=-mono-inline-il-limit=0), so a
 * fold this reads is the cost model's.
 *
 * What says a fold happened is the stack trace, the way tier2-inline-cost.cs
 * reads it: a folded body owns no code, so its frame reports the offset into
 * Root () that it was folded at, and a body that was really called reports an
 * offset into itself.
 *
 * Weigh () costs 150 on mono's answers and 335 on LLVM's, and the gap is the
 * four calls in the arm the folded type word makes dead. The suite names a
 * cold-callsite threshold of 190, which the argument bonus takes to 240 in both
 * arms, so each verdict has around ninety either way.
 *
 * Both arms raise --llvm-opt=-mono-inline-cost-il-limit, because the limit
 * counts the IL a body arrives with and this one is past the default. A body
 * the model never weighed prints no verdict at all, which reads as one it
 * accepted. Re-measure when this starts failing on one arm:
 *
 *   MONO_LLVM_JIT_TRACE=1 mono-sgen --llvm-opt=-mono-tier2-threshold=0 \
 *     --llvm-opt=-mono-inline-il-limit=0 \
 *     --llvm-opt=-mono-inline-cost-il-limit=256 \
 *     --llvm-opt=-mono-inline-cost-full \
 *     --llvm-opt=-mono-inline-cold-callsite-threshold=1 \
 *     tier2-inline-dispatch.exe
 *
 * `-mono-inline-cost-full` is what makes the printed cost the whole cost. The
 * model stops adding once it is past the budget, so the cost a plain run prints
 * is cut off there.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Shape {
	public int w;

	public Shape (int a) { w = a; }

	public virtual int Sides () { return 3; }
}

class Square : Shape {
	public Square (int a) : base (a) { }

	public override int Sides () { return 4; }
}

static class Work {
	/*
	 * Stays a call, so the arm below is work the model has to count until the
	 * type test settles which arm runs.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Slow (int x) { return x * 3 + 1; }

	/*
	 * Dispatches on a parameter the site fills with a fresh Square and asks it
	 * for its type. Both reach the receiver's vtable, which the caller's own
	 * allocation states.
	 *
	 * The test is written as reference equality rather than `==`. C# compiles
	 * `Type == Type` into a call to System.Type:op_Equality, and a constant
	 * that reaches an opaque call settles no branch.
	 */
	public static int Weigh (Shape s, bool yes)
	{
		int total = s.Sides () + s.w;

		if ((object) s.GetType () == (object) typeof (Square))
			total += s.w * 3 + 1;
		else
			total += Slow (s.w) + Slow (s.w + 1) + Slow (s.w + 2) + Slow (s.w + 3);

		if (yes)
			throw new InvalidOperationException ("weigh");

		return total;
	}
}

static class Program {
	/* Whether Weigh () had a frame at all, and whether it ran inside Root (). */
	static bool saw_weigh, folded_weigh;

	/// Whether Weigh ()'s frame covers the same code as Root ()'s.
	static bool RunsInsideRoot (Exception e)
	{
		StackTrace st = new StackTrace (e, false);
		int in_weigh = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Work" && m.Name == "Weigh")
				in_weigh = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_weigh >= 0 && in_weigh == in_root;
	}

	/*
	 * The warm-up calls all take the early return, so the profile reads the
	 * block below as cold and the model weighs its site against
	 * ColdCallSiteThreshold. That is the range this test calibrates in.
	 */
	static int Root (int n, bool throwing)
	{
		int total = n * 2;

		if (n >= 0)
			return total;

		try {
			total += Work.Weigh (new Square (n & 3), throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_weigh |= (e.StackTrace ?? "").Contains ("Work.Weigh");
			folded_weigh |= RunsInsideRoot (e);
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
		bool answers = Environment.GetEnvironmentVariable ("MONO_INLINE_POLICY") != "off";
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		// Tier 1 first and asked for, for the reason tier2-inline-cost.cs gives.
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (-2, true);

		Check (saw_weigh, "Weigh () has a frame before tier 2");
		Check (!folded_weigh, "and it runs in a body of its own before tier 2");

		// Enough calls to leave counts on the tier-1 body.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_weigh = folded_weigh = false;

		Check (want == Root (-2, true), "the answer at tier 2 is the answer before it");
		Check (saw_weigh, "Weigh () still has a frame at tier 2");

		if (answers)
			Check (folded_weigh,
				"a settled receiver folds the body that reads its vtable");
		else
			Check (!folded_weigh,
				"a settled receiver is what folds the body that reads its vtable");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
