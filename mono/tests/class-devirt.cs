using System;
using System.Runtime.CompilerServices;

/*
 * A dispatch on a receiver whose class GuardDispatchPass only guesses.
 *
 * guessed_class () reads a class off an allocation through two channels that
 * carry no proof: a field whose object escapes into another object, and a phi
 * where only one arm names a class. The guess can be wrong. GuardDispatchPass
 * (mono/llvm/passes/devirtualize.cpp) compares the receiver's real
 * vtable against the guess, calls the guessed class directly on the
 * arm that matches, and keeps the ordinary dispatch on the arm that
 * does not. mono/tests/array-devirt.cs gates the pass's older rule,
 * the one for an array receiver. This file gates the guess.
 *
 * `EscapedField` is the shape a LINQ iterator chain builds, each
 * enumerator cached in the next one's field: a Node stores its class
 * in one field, then the Node itself is stored into another Node's
 * field, which is what the walk cannot see past. `Overwrite` writes
 * that field again from outside the method the guard runs over, so
 * the guess a compile makes can disagree with what the field holds by
 * the time the read runs. `EscapedField (true)` is the negative
 * control: the guess names Alpha, the field holds Beta, and the
 * answer still has to be Beta's.
 *
 * `PhiPartial` is the other channel: a phi where one arm reads
 * KnownAlpha, an initonly static, and the other arm is a parameter,
 * whose declared class the guess rule refuses because a compare
 * against a bound would miss every subclass the parameter admits.
 *
 * `ExactAlloc` is a receiver the compile can name exactly, with
 * nothing to guess. It confirms the plain fold still takes such a
 * site before the guard ever sees it, rather than the guard wrapping
 * a compare around an answer that needed none.
 *
 * `MONO_LLVM_JIT_GUARD_CLASSES=0` turns the guess off, and every site
 * here has to answer the same either way: `runtime-class-guard` is
 * the arm whose threshold reaches the guard, and
 * `runtime-class-guard-off` runs this file again with the guess off,
 * which is the answer the guess has to agree with.
 */

public class ClassDevirt {
	static int failures;

	static void Check (string what, int got, int want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
		++failures;
	}

	abstract class Shape {
		public abstract int Which ();
	}

	sealed class Alpha : Shape {
		public override int Which () { return 1; }
	}

	sealed class Beta : Shape {
		public override int Which () { return 2; }
	}

	sealed class Gamma : Shape {
		public override int Which () { return 3; }
	}

	// The holder a receiver's class is read back out of, and the field that
	// stores it into another Node, which is what makes it escape.
	sealed class Node {
		public Shape value;
		public Node next;
	}

	// Writes h.value from outside EscapedField, so the guess EscapedField's
	// own compile makes cannot see this write. mismatch picks whether the
	// field ends up holding the class the guess names or a different one.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Overwrite (Node h, bool mismatch)
	{
		if (mismatch)
			h.value = new Beta ();
	}

	// h.value is stored once, to an Alpha, in code the guess can read. h then
	// escapes into sink.next, so the guess cannot answer for any later write
	// to h.value -- Overwrite's is one, and it runs whether or not it takes
	// the mismatch branch. The guess still names Alpha both times, because
	// that is the one store this compile can see.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int EscapedField (bool mismatch)
	{
		Node h = new Node ();

		h.value = new Alpha ();

		Node sink = new Node ();

		sink.next = h;

		Overwrite (h, mismatch);

		return h.value.Which ();
	}

	// The one arm the guess can name: an initonly static, read rather than
	// allocated here, so the optimizer has no store in this method to fold
	// the read away with before the merge -- a fresh allocation's own
	// constant store would let it do exactly that, and answer the guard a
	// vtable it never compared against.
	static readonly Shape KnownAlpha = new Alpha ();

	// One arm reads KnownAlpha, so the guess names Alpha. The other arm is a
	// parameter, whose class the guess rule will not answer from, so that
	// arm carries no guess of its own into the merge.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PhiPartial (bool known, Shape fallback)
	{
		Shape s = known ? KnownAlpha : fallback;

		return s.Which ();
	}

	// One allocation, nothing merged into it, so the plain fold answers this
	// before GuardDispatchPass runs. No compare belongs here.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ExactAlloc ()
	{
		Shape s = new Gamma ();

		return s.Which ();
	}

	static void Round ()
	{
		Check ("escaped field, guess correct", EscapedField (false), 1);
		Check ("escaped field, guess wrong", EscapedField (true), 2);
		Check ("phi partial, known arm", PhiPartial (true, new Beta ()), 1);
		Check ("phi partial, fallback arm", PhiPartial (false, new Beta ()), 2);
		Check ("exact alloc", ExactAlloc (), 3);
	}

	public static int Main ()
	{
		// The first rounds run interpreted, and the later ones run whatever the
		// thresholds promoted. Both answer through this same code. The count is
		// what gives a promotion time to land: a tier-1 compile is asynchronous,
		// and a few hundred interpreted rounds are over before one arrives.
		for (int i = 0; i < 20000; ++i)
			Round ();

		if (failures != 0) {
			Console.WriteLine ("{0} wrong answers", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
