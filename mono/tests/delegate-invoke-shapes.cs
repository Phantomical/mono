using System;
using System.Reflection;

/*
 * Every shape a delegate's invoke_impl can settle on. Four of them are served by a
 * stub that jumps through method_ptr; multicast and open-instance are not, so those
 * two are where a call site that read the wrong field still passes everything else.
 *
 * The return type is wide enough to travel through a hidden pointer, which is the
 * other thing a delegate call site has to get right.
 */

struct Big {
	public long a, b, c;

	public Big (long v) { a = v; b = v + 1; c = v + 2; }
	public long Sum { get { return a + b + c; } }
}

delegate Big BigFunc (int x);
delegate Big OpenBigFunc (Target t, int x);
delegate int IntFunc (int x);

class Target {
	public static int calls;
	public int bias;

	public Target (int bias) { this.bias = bias; }

	public Big Instance (int x) { return new Big (x + bias); }
	public static Big Static (int x) { return new Big (x + 1000); }
	public static Big ClosedStatic (Target t, int x) { return new Big (x + t.bias + 100); }
	public virtual Big Virtual (int x) { return new Big (x + 1); }

	public int Counted (int x) { calls += x; return calls; }
}

class Derived : Target {
	public Derived (int bias) : base (bias) { }

	public override Big Virtual (int x) { return new Big (x + 2); }
}

class DelegateInvokeShapes {
	/* What Big (v).Sum comes to, kept away from the compiler's constant folding. */
	static long Expected (long v) { return v + (v + 1) + (v + 2); }

	static BigFunc Nothing () { return null; }

	public static int Main ()
	{
		Target target = new Target (10);
		Derived derived = new Derived (20);

		/* static: no target, and the signatures line up */
		BigFunc stat = Target.Static;
		if (stat (5).Sum != Expected (1005))
			return 1;

		/* instance: the receiver is bound into the delegate */
		BigFunc inst = target.Instance;
		if (inst (5).Sum != Expected (15))
			return 2;

		/* virtual: the override is resolved when the delegate is built */
		BigFunc virt = derived.Virtual;
		if (virt (5).Sum != Expected (7))
			return 3;

		/* closed static: the target becomes the static method's first argument */
		BigFunc closed = (BigFunc) Delegate.CreateDelegate (
			typeof (BigFunc), target, typeof (Target).GetMethod ("ClosedStatic"));
		if (closed (5).Sum != Expected (115))
			return 4;

		/* open instance: the receiver is argument 0 and dispatch stays virtual */
		OpenBigFunc open = (OpenBigFunc) Delegate.CreateDelegate (
			typeof (OpenBigFunc), typeof (Target).GetMethod ("Virtual"));
		if (open (target, 5).Sum != Expected (6))
			return 5;
		if (open (derived, 5).Sum != Expected (7))
			return 6;

		/*
		 * Multicast: every target runs and the last one's value is the answer.
		 * Both halves are invoked on their own first, so each has settled on a
		 * single-target implementation before it is folded into one that has to
		 * run them both.
		 */
		Target.calls = 0;
		IntFunc first = target.Counted;
		IntFunc second = target.Counted;
		if (first (1) != 1 || second (1) != 2)
			return 7;

		IntFunc counted = (IntFunc) Delegate.Combine (first, second);
		if (counted (3) != 8 || Target.calls != 8)
			return 8;

		/* Taking one back out leaves a delegate that was already single-target. */
		IntFunc single = (IntFunc) Delegate.Remove (counted, second);
		if (single (2) != 10 || Target.calls != 10)
			return 9;

		BigFunc multi = (BigFunc) Delegate.Combine (stat, inst);
		if (multi (5).Sum != Expected (15))
			return 12;

		/*
		 * A second round, because the first call through each of these is the one
		 * that decides what the delegate dispatches through from then on.
		 */
		if (stat (5).Sum != Expected (1005) || inst (5).Sum != Expected (15)
		    || virt (5).Sum != Expected (7) || closed (5).Sum != Expected (115)
		    || open (target, 5).Sum != Expected (6)
		    || open (derived, 5).Sum != Expected (7)
		    || multi (5).Sum != Expected (15))
			return 13;

		/* Invoking through a null delegate throws before anything is read off it. */
		BigFunc none = Nothing ();
		try {
			none (5);
			return 14;
		} catch (NullReferenceException) {
		}

		return 0;
	}
}
