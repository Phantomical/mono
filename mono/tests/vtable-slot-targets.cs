using System;
using System.Runtime.CompilerServices;
using System.Threading;

/*
 * A vtable slot does not always stand for the code of the method the class
 * layout put there. Some slots are entered through something the runtime built
 * instead - a marshalling wrapper, a delegate trampoline, an unboxing entry -
 * and one kind of slot has no single target at all. Each case below dispatches
 * through such a slot and checks where it landed, which is what goes wrong if a
 * slot is filled with the implementing method's own address.
 */

interface IShape { string Name (); }

struct Point : IShape {
	public int X;
	public Point (int x) { X = x; }
	/* Reached with the receiver still boxed, off the struct's own vtable. */
	public string Name () { return "Point" + X; }
	public override string ToString () { return "P" + X; }
}

class Base {
	/* No single target: each instantiation is a different body, and which one
	 * is wanted only shows up in the IMT register at the call. */
	public virtual string GV<T> () { return "Base:" + typeof (T).Name; }
	public virtual string Plain () { return "Base"; }

	/* The lock is taken by the wrapper the runtime generates around this, so
	 * whether it is held says which of the two the slot led to. */
	[MethodImpl (MethodImplOptions.Synchronized)]
	public virtual string Sync () { return Monitor.IsEntered (this) ? "BaseSync" : "BaseUnlocked"; }
}

class Derived : Base {
	public override string GV<T> () { return "Derived:" + typeof (T).Name; }
	public override string Plain () { return "Derived"; }

	[MethodImpl (MethodImplOptions.Synchronized)]
	public override string Sync () { return Monitor.IsEntered (this) ? "DerivedSync" : "DerivedUnlocked"; }
}

public class Driver {
	static int failures;

	/* Past the point where a repeated dispatch would have settled. */
	const int Reps = 25;

	static void Check (string got, string want, string what)
	{
		if (got != want) {
			Console.Error.WriteLine ("{0}: got '{1}', expected '{2}'", what, got, want);
			failures++;
		}
	}

	static void Repeat (Func<string> call, string want, string what)
	{
		for (int i = 0; i < Reps; i++)
			Check (call (), want, what);
	}

	/*
	 * The one shape a slot can never hold: two instantiations of one generic
	 * virtual method share a slot, so filling it with either is wrong for the
	 * other.
	 */
	static void GenericVirtualOffAClass ()
	{
		Base b = new Derived ();

		Repeat (() => b.GV<int> (), "Derived:Int32", "Base.GV<int>");
		Repeat (() => b.GV<string> (), "Derived:String", "Base.GV<string>");
		Repeat (() => b.GV<Exception> (), "Derived:Exception", "Base.GV<Exception>");
		Check (b.GV<int> (), "Derived:Int32", "Base.GV<int> revisited");

		/* The base's own body, through the base's own slot. */
		Base plain = new Base ();
		Repeat (() => plain.GV<long> (), "Base:Int64", "Base.GV<long> on Base");
	}

	/* Entered through the synchronized wrapper, not through the method. */
	static void Synchronized ()
	{
		Base b = new Derived ();

		Repeat (() => b.Sync (), "DerivedSync", "Base.Sync");
		Repeat (() => b.Plain (), "Derived", "Base.Plain");

		Base plain = new Base ();
		Repeat (() => plain.Sync (), "BaseSync", "Base.Sync on Base");
	}

	/* Delegate Invoke is virtual and runtime-implemented: its slot is entered
	 * through a trampoline the runtime emits, never through IL. */
	static void DelegateInvoke ()
	{
		Func<int, string> f = x => "f" + x;
		Repeat (() => f (7), "f7", "Func`2.Invoke");

		Converter<int, string> c = x => "c" + x;
		Repeat (() => c (8), "c8", "Converter`2.Invoke");
	}

	/* An override implemented as an internal call, reached virtually. */
	static void InternalCallOverride ()
	{
		object t = typeof (Driver);

		Repeat (() => t.GetType ().Name, "RuntimeType", "Type.GetType().Name");
		Repeat (() => ((Type)t).Name, "Driver", "RuntimeType.get_Name");
	}

	/* A value type reached through a slot still has its receiver boxed. */
	static void BoxedReceiver ()
	{
		IShape s = new Point (3);
		object o = new Point (4);

		Repeat (() => s.Name (), "Point3", "IShape.Name on Point");
		Repeat (() => o.ToString (), "P4", "Point.ToString");
	}

	public static int Main ()
	{
		GenericVirtualOffAClass ();
		Synchronized ();
		DelegateInvoke ();
		InternalCallOverride ();
		BoxedReceiver ();

		if (failures != 0) {
			Console.Error.WriteLine ("{0} failures", failures);
			return 1;
		}
		return 0;
	}
}
