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
 *   - a null arm must never make the walk answer some class for a null;
 *   - an allocation that reaches a protected region through inlining, and so
 *     loses the mark that names its class, must still answer that class.
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

	// Small enough for the trivial-inline pre-pass to fold into any caller,
	// which is what puts this allocation inside AllocatedInATryBlock's try.
	static Shape MakeNarrow ()
	{
		return new Narrow ();
	}

	/*
	 * MakeNarrow ()'s own translation marks its allocation with an exact
	 * class, because nothing in MakeNarrow () itself is protected. Folding
	 * MakeNarrow ()'s body in here moves that allocation inside this try
	 * block, and the call to MakeNarrow () was already an invoke because it
	 * sits in a protected region - so InlineFunction () rewrites the
	 * allocation call into an invoke of its own, targeting this block's
	 * landing pad. LLVM's changeToInvokeAndSplitBasicBlock () copies the
	 * debug location, the calling convention, the attributes and MD_prof
	 * onto that invoke, and nothing else, so the exact-class mark is gone.
	 * The dispatch below has to answer from the allocation's vtable operand
	 * instead.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int AllocatedInATryBlock ()
	{
		try {
			Shape s = MakeNarrow ();

			if (s == null)
				throw new Exception ();

			return s.Which ();
		} catch (Exception) {
			return -1;
		}
	}

	sealed class Holder {
		public Shape s;
	}

	/*
	 * The receiver comes from a field, not straight off an allocation or a
	 * merge. `h.s = new Narrow ()` stores a reference, so the translator
	 * writes a write-barrier call beside the store
	 * (`MethodLLVMEmitter::emit_reference_store ()`,
	 * `method-to-llvm/fields.cpp`), taking the field's address as an
	 * argument. A walk that answered a class off the field's one store still
	 * had to read past that call to reach it.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FieldReceiver ()
	{
		Holder h = new Holder ();
		h.s = new Narrow ();
		return h.s.Which ();
	}

	/*
	 * The field's own base is a merge of two allocations rather than one.
	 * Both branches make their own `Holder` and store a `Narrow` into it, so
	 * the receiver a caller reads off `.s` is a load whose base a phi names,
	 * and only both incoming `Holder`s' own field stores together settle it.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FieldOfAMergedReceiver (bool which)
	{
		Holder h = which ? new Holder { s = new Narrow () } : new Holder { s = new Narrow () };
		return h.s.Which ();
	}

	sealed class DoubleHolder {
		public Holder h;
	}

	/*
	 * The receiver is a field of a field. `d.h` is itself a load, so
	 * resolving `d.h.s`'s base needs `d.h`'s own single store read back
	 * before `.s` can be answered at all.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FieldOfAFieldReceiver ()
	{
		DoubleHolder d = new DoubleHolder ();
		d.h = new Holder ();
		d.h.s = new Narrow ();
		return d.h.s.Which ();
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

		Check (tier + ": allocated in a try block", AllocatedInATryBlock (), 1);

		Check (tier + ": field receiver", FieldReceiver (), 1);

		Check (tier + ": field of a merged receiver, true arm", FieldOfAMergedReceiver (true), 1);
		Check (tier + ": field of a merged receiver, false arm", FieldOfAMergedReceiver (false), 1);

		Check (tier + ": field of a field receiver", FieldOfAFieldReceiver (), 1);
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
		       & Promote ("LoopReceiver", tier) & Promote ("IsNarrow", tier)
		       & Promote ("AllocatedInATryBlock", tier) & Promote ("FieldReceiver", tier)
		       & Promote ("FieldOfAMergedReceiver", tier) & Promote ("FieldOfAFieldReceiver", tier);
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
