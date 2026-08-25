using System;
using System.Runtime.Serialization;

// ECMA-335 I.8.9.5 does not make entry into an instance method of a reference
// type a trigger for the type initializer, because a non-null instance can only
// come from a constructor and the constructor is a trigger. The backend leaves
// the check out of such an entry. Each case below is a way to reach an instance
// method whose class holds statics, and each one has to read them initialized.
//
// Value types stay on the list, so a struct keeps its check. An all-zero struct
// needs no constructor, which is the shape Zeroed () builds.

class Log {
	public static int PlainRuns;
	public static int GenericRuns;
	public static int BaseRuns;
	public static int StructRuns;
	public static int RawRuns;
	public static int ActivatedRuns;
}

class Plain {
	public static int Value;

	static Plain ()
	{
		Log.PlainRuns++;
		Value = 11;
	}

	public int Read () { return Value; }
}

// The case the check costs the most: a shared body reads its context out of the
// receiver, so the check it carries fetches a vtable through the rgctx first.
class Generic<T> {
	public static int Value;

	static Generic ()
	{
		Log.GenericRuns++;
		Value = 22;
	}

	public int Read () { return Value; }
	public static int ReadStatic () { return Value; }
}

class Base {
	public static int Value;

	static Base ()
	{
		Log.BaseRuns++;
		Value = 33;
	}

	public int Read () { return Value; }
}

// Base's initializer is not triggered by Derived's. It runs because Derived's
// constructor chains to Base's, which is a constructor for Base.
class Derived : Base {
}

struct Zero {
	public static int Value;

	static Zero ()
	{
		Log.StructRuns++;
		Value = 44;
	}

	// Touches no static, so entering it is the only thing that can run the
	// initializer. A method that read Value would be covered by the check that
	// static field access carries, whatever the entry does.
	public int Touch () { return 44; }
}

class Raw {
	public static int Value;

	static Raw ()
	{
		Log.RawRuns++;
		Value = 55;
	}

	public int Read () { return Value; }
}

class Activated {
	public static int Value;

	static Activated ()
	{
		Log.ActivatedRuns++;
		Value = 66;
	}

	// Touches no static, for the reason Zero.Touch () gives.
	public int Touch () { return 66; }
}

class CctorInitInstance {
	// Enough turns to promote past tier 0, so the compiled body is what answers.
	const int Turns = 200;

	static int Instance ()
	{
		Plain p = new Plain ();
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += p.Read ();

		return total;
	}

	static int Shared ()
	{
		Generic<string> g = new Generic<string> ();
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += g.Read ();

		return total;
	}

	static int Inherited ()
	{
		Base b = new Derived ();
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += b.Read ();

		return total;
	}

	// An all-zero struct, which C# builds with initobj and no constructor. So
	// entering Touch () is the first thing that can initialize Zero.
	static int Zeroed ()
	{
		Zero z = new Zero ();
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += z.Touch ();

		return total;
	}

	// GetUninitializedObject is the one way to hold an instance of a class whose
	// constructor never ran, so it owes the initializer the constructor would
	// have run.
	static int Uninitialized ()
	{
		Raw r = (Raw) FormatterServices.GetUninitializedObject (typeof (Raw));
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += r.Read ();

		return total;
	}

	// A constructor reached with no newobj site in front of it. Several things
	// can run the initializer on this path, so what the case pins down is the
	// end result rather than which check did it.
	static int Activate ()
	{
		Activated a = (Activated) Activator.CreateInstance (typeof (Activated));
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += a.Touch ();

		return total;
	}

	static int Check (string what, int got, int want, int code)
	{
		if (got == want)
			return 0;

		Console.WriteLine ("{0} = {1}, expected {2}", what, got, want);
		return code;
	}

	static int Main ()
	{
		int bad = Check ("Instance ()", Instance (), 11 * Turns, 1);
		if (bad != 0) return bad;

		bad = Check ("Shared ()", Shared (), 22 * Turns, 2);
		if (bad != 0) return bad;

		bad = Check ("Inherited ()", Inherited (), 33 * Turns, 3);
		if (bad != 0) return bad;

		bad = Check ("Zeroed ()", Zeroed (), 44 * Turns, 4);
		if (bad != 0) return bad;

		// Zeroed () read no static of Zero, so the entry to Touch () is what
		// this count is over.
		bad = Check ("Zero's cctor after Zeroed ()", Log.StructRuns, 1, 12);
		if (bad != 0) return bad;

		bad = Check ("Uninitialized ()", Uninitialized (), 55 * Turns, 5);
		if (bad != 0) return bad;

		bad = Check ("Activate ()", Activate (), 66 * Turns, 13);
		if (bad != 0) return bad;

		bad = Check ("Activated's cctor runs", Log.ActivatedRuns, 1, 14);
		if (bad != 0) return bad;

		// A static method of a shared class is still a trigger.
		bad = Check ("Generic<int>.ReadStatic ()", Generic<int>.ReadStatic (), 22, 6);
		if (bad != 0) return bad;

		bad = Check ("Plain's cctor runs", Log.PlainRuns, 1, 7);
		if (bad != 0) return bad;

		// One run for each instantiation: a shared body serves both, but the
		// statics and the vtable behind them are each instantiation's own.
		bad = Check ("Generic's cctor runs", Log.GenericRuns, 2, 8);
		if (bad != 0) return bad;

		bad = Check ("Base's cctor runs", Log.BaseRuns, 1, 9);
		if (bad != 0) return bad;

		bad = Check ("Zero's cctor runs", Log.StructRuns, 1, 10);
		if (bad != 0) return bad;

		bad = Check ("Raw's cctor runs", Log.RawRuns, 1, 11);
		if (bad != 0) return bad;

		return 0;
	}
}
