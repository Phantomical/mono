using System;

/*
 * What a compile reads off a class's vtable when the IR already names it.
 *
 * Every case here asserts the value rather than the shape, so it fails on a
 * wrong fold and not on a missing one. Each receiver is a fresh allocation, so
 * the vtable store in front of the read names the class and the reads fold.
 */

class Shape {
	public virtual int Sides () { return 0; }
	public virtual string Name () { return "shape"; }
}

class Square : Shape {
	public override int Sides () { return 4; }
	public override string Name () { return "square"; }
}

class Box<T> {
	public virtual int Sides () { return 0; }
	public virtual string Pick<U> (U held) { return "box:" + typeof (U).Name; }
}

class Crate<T> : Box<T> {
	public override int Sides () { return 6; }
	public override string Pick<U> (U held) { return "crate:" + typeof (U).Name; }
}

struct Counted {
	public int Value;

	public override string ToString () { return "counted:" + Value; }
}

class VTableFold {
	static int failures;

	static void Check (string what, object got, object want)
	{
		if (!got.Equals (want)) {
			Console.WriteLine ("{0}: got {1}, expected {2}", what, got, want);
			failures++;
		}
	}

	// A virtual the IL does not settle, on a receiver the allocation names. The
	// slot is what says which override runs.
	static void Dispatch ()
	{
		Shape s = new Square ();

		Check ("dispatch", s.Sides (), 4);
		Check ("dispatch-string", s.Name (), "square");
	}

	// A virtual generic. One slot serves every instantiation, so the call site's
	// own key is what picks one.
	static void GenericDispatch ()
	{
		Box<int> b = new Crate<int> ();

		Check ("generic-dispatch", b.Sides (), 6);
		Check ("generic-virtual", b.Pick<string> ("x"), "crate:String");
		Check ("generic-virtual-2", b.Pick<long> (1), "crate:Int64");
	}

	// A value type's slot holds the unbox entry, so a receiver reaching the body
	// with its box still on would read the wrong bytes.
	static void BoxedValueType ()
	{
		Counted c = new Counted ();

		c.Value = 44;

		object boxed = c;

		Check ("unbox-tostring", boxed.ToString (), "counted:44");
		Check ("unbox-cast", (int) (object) 44, 44);
	}

	// The rank and the class word an array store check reads.
	static void ArrayStore ()
	{
		object[] items = new Shape[4];

		items[0] = new Square ();
		Check ("array-store", ((Shape) items[0]).Sides (), 4);
		Check ("array-length", items.Length, 4);
	}

	// The System.Type a vtable carries is one object, so a GetType () and a
	// typeof name the same one. A collection between the two would move a
	// constant that was not pinned.
	static void TypeIdentity ()
	{
		Square s = new Square ();
		Type held = s.GetType ();

		GC.Collect ();
		GC.WaitForPendingFinalizers ();

		Check ("type-identity", (object) held == (object) typeof (Square), true);
		Check ("type-name", held.Name, "Square");
		Check ("type-after-gc", s.GetType ().Name, "Square");
	}

	static int Main ()
	{
		// Many turns, so every body promotes past tier 0 and the compiled
		// answers are what the checks read.
		for (int i = 0; i < 200000; i++) {
			Dispatch ();
			GenericDispatch ();
			BoxedValueType ();
			ArrayStore ();
		}

		TypeIdentity ();

		Console.WriteLine (failures == 0 ? "OK" : failures + " failures");
		return failures;
	}
}
