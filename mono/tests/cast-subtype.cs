using System;
using System.Runtime.CompilerServices;

/*
 * The answers a cast gives, over the shapes the inline supertype test in
 * generated code either answers or declines.
 *
 * That test is one-sided. Where the object's class holds the target among its
 * supertypes it answers yes, and everything else falls through to the runtime.
 * So the cases that matter are the ones where the two disagree about which of
 * them answers: a target too deep for the object, an interface, a
 * marshal-by-ref class, an array, a delegate, a boxed value and a shared
 * generic body. Each must give the answer the runtime gives, whichever path
 * carries it.
 *
 * Every probe is entered enough times to reach both compiled tiers, because
 * the interpreter answers a cast through the runtime and never reaches the
 * generated test at all.
 */

interface IAlpha { }
interface IBeta { }

class L1 { }
class L2 : L1 { }
class L3 : L2, IAlpha { }
class L4 : L3 { }

sealed class Sealed : L2 { }

class Sibling : L1 { }

class Remote : MarshalByRefObject { }
class RemoteSub : Remote { }

delegate int Op (int n);

struct Val { public int n; }

class Holder<T> where T : class {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static T As (object o) { return o as T; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static object Cast (object o) { return (T) o; }
}

static class Program {
	static int fails;

	static void Check (bool ok, string what)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	static void CheckThrows (Func<object> f, string what)
	{
		try {
			f ();
		} catch (InvalidCastException) {
			return;
		}

		Console.WriteLine ("FAIL: {0} did not throw", what);
		++fails;
	}

	/* Up the chain, where the supertypes hold the target. */
	[MethodImpl (MethodImplOptions.NoInlining)] static L1 AsL1 (object o) { return o as L1; }
	[MethodImpl (MethodImplOptions.NoInlining)] static L2 AsL2 (object o) { return o as L2; }
	[MethodImpl (MethodImplOptions.NoInlining)] static L3 AsL3 (object o) { return o as L3; }
	[MethodImpl (MethodImplOptions.NoInlining)] static L4 AsL4 (object o) { return o as L4; }
	[MethodImpl (MethodImplOptions.NoInlining)] static object CastL2 (object o) { return (L2) o; }

	/* The shapes the inline test declines. */
	[MethodImpl (MethodImplOptions.NoInlining)] static IAlpha AsAlpha (object o) { return o as IAlpha; }
	[MethodImpl (MethodImplOptions.NoInlining)] static IBeta AsBeta (object o) { return o as IBeta; }
	[MethodImpl (MethodImplOptions.NoInlining)] static Remote AsRemote (object o) { return o as Remote; }
	[MethodImpl (MethodImplOptions.NoInlining)] static object[] AsObjArray (object o) { return o as object[]; }
	[MethodImpl (MethodImplOptions.NoInlining)] static string[] AsStrArray (object o) { return o as string[]; }
	[MethodImpl (MethodImplOptions.NoInlining)] static Op AsOp (object o) { return o as Op; }
	[MethodImpl (MethodImplOptions.NoInlining)] static object AsBoxedInt (object o) { return o is int ? o : null; }
	[MethodImpl (MethodImplOptions.NoInlining)] static Sealed AsSealed (object o) { return o as Sealed; }
	[MethodImpl (MethodImplOptions.NoInlining)] static object AsObject (object o) { return o as object; }

	static int Run (int i)
	{
		object l1 = new L1 (), l2 = new L2 (), l3 = new L3 (), l4 = new L4 ();
		object sealed_one = new Sealed (), sibling = new Sibling ();
		object remote = new RemoteSub ();
		object strings = new string [2];
		object objects = new object [2];
		object op = (Op) (n => n + 1);
		object boxed = (object) new Val { n = i };
		object boxed_int = (object) i;

		/* A class holds every class above it, and the deepest answers all. */
		Check (AsL1 (l4) != null, "L4 as L1");
		Check (AsL2 (l4) != null, "L4 as L2");
		Check (AsL3 (l4) != null, "L4 as L3");
		Check (AsL4 (l4) != null, "L4 as L4");

		/*
		 * A class shallower than the target. The inline test compares the
		 * depth first, because indexing the supertypes at the target's depth
		 * would otherwise read past the end of the array.
		 */
		Check (AsL4 (l1) == null, "L1 as L4");
		Check (AsL3 (l2) == null, "L2 as L3");
		Check (AsL2 (l1) == null, "L1 as L2");

		/* Equal depth, different branch. */
		Check (AsL2 (sibling) == null, "a sibling at the same depth as L2");
		Check (AsL1 (sibling) != null, "a sibling as their shared base");

		/* Sealed, and a class that is not related at all. */
		Check (AsSealed (sealed_one) != null, "Sealed as Sealed");
		Check (AsSealed (l4) == null, "L4 as Sealed");
		Check (AsL2 (sealed_one) != null, "Sealed as its base");

		/* Interfaces, which live in the bitmap rather than the supertypes. */
		Check (AsAlpha (l3) != null, "L3 as IAlpha");
		Check (AsAlpha (l4) != null, "L4 as IAlpha, inherited");
		Check (AsAlpha (l2) == null, "L2 as IAlpha");
		Check (AsBeta (l4) == null, "L4 as IBeta");

		/* Marshal-by-ref, which the runtime answers through remoting. */
		Check (AsRemote (remote) != null, "RemoteSub as Remote");
		Check (AsRemote (l1) == null, "L1 as Remote");

		/* Arrays and their covariance. */
		Check (AsObjArray (strings) != null, "string[] as object[]");
		Check (AsStrArray (strings) != null, "string[] as string[]");
		Check (AsStrArray (objects) == null, "object[] as string[]");
		Check (AsObjArray (l1) == null, "a class as object[]");

		Check (AsOp (op) != null, "a delegate as its own type");
		Check (AsOp (l1) == null, "a class as a delegate");

		/* A boxed value, whose class is a value type. */
		Check (AsBoxedInt (boxed_int) != null, "a boxed int is an int");
		Check (AsBoxedInt (boxed) == null, "a boxed struct is not an int");
		Check (AsObject (boxed) != null, "a boxed struct as object");
		Check (AsObject (l4) != null, "a class as object");

		/* Null answers null for every target, and reads no vtable. */
		Check (AsL1 (null) == null, "null as L1");
		Check (AsAlpha (null) == null, "null as IAlpha");
		Check (AsObjArray (null) == null, "null as object[]");
		Check (AsSealed (null) == null, "null as Sealed");

		/* A shared generic body reaches its target through the context. */
		Check (Holder<L2>.As (l4) != null, "shared: L4 as L2");
		Check (Holder<L4>.As (l2) == null, "shared: L2 as L4");
		Check (Holder<L1>.Cast (l3) != null, "shared: cast L3 to L1");

		/* The throwing form, on both sides of the answer. */
		Check (CastL2 (l4) != null, "cast L4 to L2");
		Check (CastL2 (null) == null, "cast null to L2");

		return fails;
	}

	public static int Main ()
	{
		/* Enough entries to leave the interpreter and both compiled tiers. */
		for (int i = 0; i < 30000; ++i) {
			if (Run (i) != 0) {
				Console.WriteLine ("stopped at iteration {0}", i);
				return 1;
			}
		}

		/* The throwing arm, which is too slow to run in the loop above. */
		object l1 = new L1 ();

		CheckThrows (() => (L2) l1, "cast L1 to L2");
		CheckThrows (() => (IBeta) l1, "cast L1 to IBeta");
		CheckThrows (() => (string[]) (object) new object [1], "cast object[] to string[]");
		CheckThrows (() => Holder<L4>.Cast (new L2 ()), "shared cast L2 to L4");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
