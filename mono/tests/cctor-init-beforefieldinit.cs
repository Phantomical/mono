using System;

// A type marked beforefieldinit has one trigger for its initializer: the first
// access to a static field of the type (ECMA-335 I.8.9.5). Entering one of its
// methods is not a trigger, so the backend leaves the check out of the entry
// and each static field access carries one instead. C# marks a class
// beforefieldinit unless it declares an explicit static constructor, so every
// class here is one.
//
// What the cases check is that each way of reaching a static still reads it
// initialized, and that the initializer still runs exactly once.

class Log {
	public static int PlainRuns;
	public static int GenericRuns;
	public static int WriteRuns;
	public static int AddrRuns;
	public static int SpecialRuns;
	public static int FoldedRuns;
	public static int BoomRuns;
}

class Plain {
	public static int Value = Bump ();

	static int Bump ()
	{
		Log.PlainRuns++;
		return 11;
	}

	// Static and instance, neither of them a trigger. Both read the field, and
	// that read is.
	public static int ReadStatic () { return Value; }
	public int ReadInstance () { return Value; }
}

// The shared case: one body serves every reference instantiation, and the
// statics it reads are each instantiation's own.
class Generic<T> {
	public static int Value = Bump ();

	static int Bump ()
	{
		Log.GenericRuns++;
		return 22;
	}

	public int ReadInstance () { return Value; }
	public static int ReadStatic () { return Value; }
}

class Written {
	public static int Value = Bump ();

	static int Bump ()
	{
		Log.WriteRuns++;
		return 33;
	}

	// stsfld is a static field access, so it triggers as much as a read does.
	// A write that ran before the initializer would be overwritten by it.
	public static int Overwrite ()
	{
		Value = 99;
		return Value;
	}
}

class Addressed {
	public static int Value = Bump ();

	static int Bump ()
	{
		Log.AddrRuns++;
		return 44;
	}

	// A byref to a static field is ldsflda, the third way to reach the statics
	// block.
	public static int ReadThroughAddress () { return Deref (ref Value); }

	static int Deref (ref int slot) { return slot; }
}

struct Zero {
	public static int Value = 55;

	// A value type's instance method is a trigger only when the type is not
	// beforefieldinit, so this one is reached with the entry check gone. The
	// read is what initializes.
	public int Read () { return Value; }
}

// A thread-static goes down the special-static arm of the same three emitters,
// which is the arm a check is easiest to miss on. ReadSlot () is a method of
// this class, so the arm under test is the field access rather than anything
// the engines do at entry.
class Special {
	[ThreadStatic] public static int Slot;
	public static int Marker = Init ();

	static int Init ()
	{
		Log.SpecialRuns++;
		Slot = 66;
		return 1;
	}

	public static int ReadSlot () { return Slot; }
}

// One straight line then ret, and small, so the trivial-callee pre-pass folds
// Get () into its caller. The folded copy has to carry the check with it.
class Folded {
	public static int Value = Bump ();

	static int Bump ()
	{
		Log.FoldedRuns++;
		return 77;
	}

	public static int Get () { return Value; }
}

class Producer {
	public static int Value = 100;
}

// Its initializer reads another class's static, so one initializer runs inside
// the other.
class Dependent {
	public static int Value = Producer.Value + 88;
}

// A field initializer that throws keeps the class beforefieldinit, where an
// explicit static constructor would not. With no check at the entry, the
// failure is owed at the field access instead.
class Boom {
	public static int Value = Detonate ();

	static int Detonate ()
	{
		Log.BoomRuns++;
		throw new InvalidOperationException ("boom");
	}
}

class BeforeFieldInit {
	// Enough turns to promote past tier 0, so the compiled body is what answers.
	const int Turns = 200;

	static int Static ()
	{
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += Plain.ReadStatic ();

		return total;
	}

	static int Instance ()
	{
		Plain p = new Plain ();
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += p.ReadInstance ();

		return total;
	}

	static int Shared ()
	{
		Generic<string> g = new Generic<string> ();
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += g.ReadInstance ();

		return total;
	}

	static int Write ()
	{
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += Written.Overwrite ();

		return total;
	}

	static int Address ()
	{
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += Addressed.ReadThroughAddress ();

		return total;
	}

	static int Struct ()
	{
		Zero z = new Zero ();
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += z.Read ();

		return total;
	}

	static int Special_ ()
	{
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += Special.ReadSlot ();

		return total;
	}

	static int Fold ()
	{
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += Folded.Get ();

		return total;
	}

	static int Nested ()
	{
		int total = 0;

		for (int i = 0; i < Turns; i++)
			total += Dependent.Value;

		return total;
	}

	// The first access has to raise TypeInitializationException, and every later
	// one has to raise it again off the cached failure without rerunning the
	// initializer.
	static int Throwing ()
	{
		int raised = 0;

		for (int i = 0; i < Turns; i++) {
			try {
				raised += Boom.Value;
			} catch (TypeInitializationException) {
				raised++;
			}
		}

		return raised;
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
		int bad = Check ("Static ()", Static (), 11 * Turns, 1);
		if (bad != 0) return bad;

		bad = Check ("Instance ()", Instance (), 11 * Turns, 2);
		if (bad != 0) return bad;

		bad = Check ("Shared ()", Shared (), 22 * Turns, 3);
		if (bad != 0) return bad;

		// 99 every turn: the check runs before the store. If it ran after
		// instead, the initializer's own assignment would overwrite the first
		// write.
		bad = Check ("Write ()", Write (), 99 * Turns, 4);
		if (bad != 0) return bad;

		bad = Check ("Address ()", Address (), 44 * Turns, 5);
		if (bad != 0) return bad;

		bad = Check ("Struct ()", Struct (), 55 * Turns, 6);
		if (bad != 0) return bad;

		// A static method of a value-type instantiation, which compiles its own
		// body rather than sharing one.
		bad = Check ("Generic<int>.ReadStatic ()", Generic<int>.ReadStatic (), 22, 7);
		if (bad != 0) return bad;

		bad = Check ("Plain's cctor runs", Log.PlainRuns, 1, 8);
		if (bad != 0) return bad;

		// One run for each instantiation: a shared body serves both, but the
		// statics behind it are each instantiation's own.
		bad = Check ("Generic's cctor runs", Log.GenericRuns, 2, 9);
		if (bad != 0) return bad;

		bad = Check ("Written's cctor runs", Log.WriteRuns, 1, 10);
		if (bad != 0) return bad;

		bad = Check ("Addressed's cctor runs", Log.AddrRuns, 1, 11);
		if (bad != 0) return bad;

		bad = Check ("Special_ ()", Special_ (), 66 * Turns, 12);
		if (bad != 0) return bad;

		bad = Check ("Special's cctor runs", Log.SpecialRuns, 1, 13);
		if (bad != 0) return bad;

		bad = Check ("Fold ()", Fold (), 77 * Turns, 14);
		if (bad != 0) return bad;

		bad = Check ("Folded's cctor runs", Log.FoldedRuns, 1, 15);
		if (bad != 0) return bad;

		bad = Check ("Nested ()", Nested (), 188 * Turns, 16);
		if (bad != 0) return bad;

		bad = Check ("Throwing ()", Throwing (), Turns, 17);
		if (bad != 0) return bad;

		// The failure is cached, so the initializer ran once however many
		// accesses asked for it.
		bad = Check ("Boom's cctor runs", Log.BoomRuns, 1, 18);
		if (bad != 0) return bad;

		return 0;
	}
}
