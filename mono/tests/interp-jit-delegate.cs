using System;

// Delegates built in a frame one engine ran, over targets the other engine may
// run. `ldftn` pushes a method pointer and `newobj Delegate::.ctor` consumes
// it, and the two have to agree on what that pointer is even when the frame
// holding them is not the engine the rest of the process uses.
//
// Every delegate here is created inside Creator, so running this with
// `--interpreter --interp=jit=Creator` compiles exactly the creating frames and
// interprets everything else.

public delegate int BinOp (int a, int b);
public delegate int UnOp (int a);

public class Target {
	int factor;

	public Target (int factor) { this.factor = factor; }

	public static int Add (int a, int b) { return a + b; }
	public int Scale (int a, int b) { return factor * (a + b); }
	public virtual int Bump (int a) { return a + 1; }
}

public class Derived : Target {
	public Derived () : base (0) { }

	public override int Bump (int a) { return a + 2; }
}

public class Creator {
	public static int Static () { BinOp d = Target.Add; return d (3, 4); }

	public static int Instance () { BinOp d = new Target (2).Scale; return d (3, 4); }

	public static int Lambda () { UnOp d = x => x + 1; return d (41); }

	public static int Virtual () { UnOp d = new Derived ().Bump; return d (40); }

	public static int Multicast ()
	{
		BinOp d = Target.Add;
		d += Target.Add;
		return d (3, 4);
	}

	// A delegate bound to another delegate's Invoke, which the runtime has to
	// redirect to that delegate's invoke wrapper.
	public static int OverInvoke ()
	{
		BinOp inner = Target.Add;
		BinOp outer = inner.Invoke;
		return outer (3, 4);
	}

	// A method pointer that is called rather than stored in a delegate.
	public static int Reflected ()
	{
		BinOp d = (BinOp)Delegate.CreateDelegate (typeof (BinOp), typeof (Target).GetMethod ("Add"));
		return d (3, 4);
	}
}

public class InterpJitDelegate {
	static int failures;

	static void Check (string what, int got, int expected)
	{
		if (got != expected) {
			Console.WriteLine (what + ": got " + got + ", expected " + expected);
			failures++;
		}
	}

	public static int Main ()
	{
		Check ("static", Creator.Static (), 7);
		Check ("instance", Creator.Instance (), 14);
		Check ("lambda", Creator.Lambda (), 42);
		Check ("virtual", Creator.Virtual (), 42);
		Check ("multicast", Creator.Multicast (), 7);
		Check ("over-invoke", Creator.OverInvoke (), 7);
		Check ("reflected", Creator.Reflected (), 7);
		return failures;
	}
}
