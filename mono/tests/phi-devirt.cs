using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * A virtual dispatch through a merged receiver -- a phi or a select -- can
 * fold to a direct call. It folds only where exact_class () and
 * operand_class () (mono/llvm/operand-class.cpp) answer one class for the
 * receiver, and it falls back to an ordinary dispatch everywhere else.
 *
 * The receiver in each case below is a value two or more paths write. Each
 * case gates one rule the merge walk must get right:
 *   - agreement folds;
 *   - a disagreement must not;
 *   - a value carried around a loop must not misread the loop;
 *   - a null arm must never make the walk answer some class for a null.
 *
 * A wrong fold here is not a slower answer. It is a wrong one. Narrow and
 * Wide read a different number of fields, so a dispatch to the wrong
 * override reads whatever memory lies past the shorter object.
 *
 * Mono.Tiering.MonoTier::PromoteNow compiles a method at the tier it is
 * given, on this thread, so the test needs no environment and races no
 * compile worker.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

abstract class Shape {
	public abstract int Which ();
}

sealed class Narrow : Shape {
	long a;

	public override int Which () { return (int) (a + 1); }
}

sealed class Wide : Shape {
	long a, b, c, d;

	public override int Which () { return (int) (a + b + c + d + 2); }
}

public static class Program {
	static int fails;

	static void Check (string what, long got, long want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL: {0}: got {1}, want {2}", what, got, want);
		++fails;
	}

	static void Check (string what, bool got, bool want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL: {0}: got {1}, want {2}", what, got, want);
		++fails;
	}

	// Two allocations of one class, merged into one receiver. Every path
	// names Narrow, so a dispatch either way has to answer 1.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SameClass (bool which)
	{
		Shape s = which ? (Shape) new Narrow () : (Shape) new Narrow ();
		return s.Which ();
	}

	// Two allocations of different classes. The merge must not answer either
	// one, because a fold that guesses wrong calls the wrong override on
	// whichever arm it misses.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int DifferentClasses (bool pickNarrow)
	{
		Shape s = pickNarrow ? (Shape) new Narrow () : (Shape) new Wide ();
		return s.Which ();
	}

	/*
	 * A receiver carried around a loop. `cached` and the value the loop
	 * dispatches on name each other. `cached` is the loop header's phi. Its
	 * incoming values are itself, from the back edge, and null, from entry.
	 * `current` is what `??` merges from `cached` and a fresh allocation.
	 *
	 * That mutual cycle is the shape that let a LINQ iterator chain
	 * (`LinqDevirt:ListLocal`) reach three unresolved dispatches. The walk
	 * that answers a merge's class used to give up on any phi.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int LoopReceiver (int rounds)
	{
		Shape cached = null;
		int total = 0;

		for (int i = 0; i < rounds; i++) {
			Shape current = cached ?? new Narrow ();
			total += current.Which ();
			cached = current;
		}

		return total;
	}

	// A phi that admits null must still answer null for the null arm, whether
	// or not the other arms agree on a class.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsNarrow (int which)
	{
		object o = which == 0 ? null : which == 1 ? (object) new Narrow () : (object) new Wide ();
		return o is Narrow;
	}

	static void CheckAll (string tier)
	{
		Check (tier + ": same class, true arm", SameClass (true), 1);
		Check (tier + ": same class, false arm", SameClass (false), 1);

		Check (tier + ": different classes, Narrow arm", DifferentClasses (true), 1);
		Check (tier + ": different classes, Wide arm", DifferentClasses (false), 2);

		Check (tier + ": loop receiver", LoopReceiver (5), 5);

		Check (tier + ": is Narrow, null arm", IsNarrow (0), false);
		Check (tier + ": is Narrow, Narrow arm", IsNarrow (1), true);
		Check (tier + ": is Narrow, Wide arm", IsNarrow (2), false);
	}

	static bool Promote (string name, int tier)
	{
		MethodInfo method = typeof (Program).GetMethod (name,
			BindingFlags.Static | BindingFlags.NonPublic);

		if (Mono.Tiering.MonoTier.PromoteNow (method.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: {0} () would not compile at tier {1}", name, tier);
		++fails;
		return false;
	}

	static bool PromoteAll (int tier)
	{
		return Promote ("SameClass", tier) & Promote ("DifferentClasses", tier)
		       & Promote ("LoopReceiver", tier) & Promote ("IsNarrow", tier);
	}

	public static int Main ()
	{
		CheckAll ("first call");

		if (!PromoteAll (2))
			return 1;
		CheckAll ("tier 1");

		if (!PromoteAll (3))
			return 1;
		CheckAll ("tier 2");

		return fails == 0 ? 0 : 1;
	}
}
