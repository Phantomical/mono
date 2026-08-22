using System;
using System.Runtime.CompilerServices;

/*
 * The answers a virtual call gives where the IL settles the receiver's class,
 * over the shapes the compiled tiers turn into a direct call.
 *
 * Two shapes settle it. A sealed class has no subclass for the receiver to be,
 * and a newobj feeding a call in the same body names the class it allocated.
 * The site then enters the implementation the receiver's vtable holds, which
 * makes the cases that matter the ones where that implementation is not the
 * method the IL named: an override on the sealed class, a method the sealed
 * class inherits and does not override, and an interface member reached
 * through the class that implements it. A null receiver must still throw. Two
 * receivers must still dispatch whatever the IL says: one whose class is open,
 * and one whose sealed class a transparent proxy can stand in for.
 *
 * Every probe is entered enough times to reach both compiled tiers. The
 * interpreter dispatches every one of these off the receiver, so it never
 * reaches the resolution being tested.
 */

interface IName {
	string Name ();
}

interface IHidden {
	string Hidden ();
}

class Base {
	public virtual string Which () { return "Base"; }
	public virtual string Kept () { return "Base.Kept"; }
}

class Middle : Base {
	public override string Which () { return "Middle"; }
}

/* A sealed class that overrides one inherited virtual method and not another. */
sealed class SealedLeaf : Middle, IName {
	public override string Which () { return "SealedLeaf"; }
	public string Name () { return "SealedLeaf.Name"; }
}

/* A sealed class whose interface member is an explicit implementation, so the
 * member sits in a slot of its own rather than under a public method. */
sealed class SealedHidden : IHidden {
	string IHidden.Hidden () { return "SealedHidden.Hidden"; }
	public string Hidden () { return "public Hidden"; }
}

/* Unsealed, so only a newobj feeding the call settles its class. */
class Open : Base {
	public override string Which () { return "Open"; }
}

class OpenSub : Open {
	public override string Which () { return "OpenSub"; }
}

/* A delegate is a sealed class whose Invoke the runtime answers off the
 * receiver rather than out of the vtable. */
delegate string Op (int n);

class GenBase {
	public virtual string Echo<T> (T value) { return "GenBase:" + value; }
}

sealed class SealedGen : GenBase {
	public override string Echo<T> (T value) { return "SealedGen:" + value; }
}

class RemoteBase : MarshalByRefObject {
	public virtual string Ping () { return "RemoteBase"; }
}

/* Sealed, and still not settled: a transparent proxy can stand in for it, and
 * remote activation decides that while the program runs. */
sealed class SealedRemote : RemoteBase {
	public override string Ping () { return "SealedRemote"; }
}

static class Program {
	static int fails;

	static void Check (string got, string want, string what)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL: {0}: got {1}, expected {2}", what, got, want);
		++fails;
	}

	static void CheckNullThrows (Action f, string what)
	{
		try {
			f ();
		} catch (NullReferenceException) {
			return;
		}

		Console.WriteLine ("FAIL: {0} did not throw", what);
		++fails;
	}

	/* A sealed receiver, whose class has the answer whatever the IL named. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string SealedOverride (SealedLeaf o) { return o.Which (); }

	/* The same receiver, on a method the sealed class does not override. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string SealedInherited (SealedLeaf o) { return o.Kept (); }

	/* An interface member reached through the sealed class that holds it. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string SealedInterface (SealedLeaf o) { return o.Name (); }

	/* The same, where the implementation is explicit and private. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string SealedExplicit (SealedHidden o) { return ((IHidden) o).Hidden (); }

	/* A generic virtual method, which dispatches through a trampoline until
	 * the receiver's class settles it. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string SealedGeneric (SealedGen o) { return o.Echo (7); }

	/* A sealed marshal-by-ref receiver, which keeps its dispatch. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string SealedByRef (SealedRemote o) { return o.Ping (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string ThroughDelegate (Op op) { return op (3); }

	/* A newobj feeding the call, with no sealed class anywhere in it. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string FromNewObj () { return new Open ().Which (); }

	/* The receiver's class stays open here, so the site keeps dispatching. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string Dispatched (Base o) { return o.Which (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string NullSealed (SealedLeaf o) { return o.Which (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string NullInterface (SealedHidden o) { return ((IHidden) o).Hidden (); }

	static string Doubled (int n) { return "Op:" + (n * 2); }

	static readonly Op op = Doubled;

	static int Run ()
	{
		SealedLeaf leaf = new SealedLeaf ();
		SealedHidden hidden = new SealedHidden ();
		SealedGen generic = new SealedGen ();
		SealedRemote remote = new SealedRemote ();

		Check (SealedOverride (leaf), "SealedLeaf", "sealed class overrides Which");
		Check (SealedInherited (leaf), "Base.Kept", "sealed class inherits Kept");
		Check (SealedInterface (leaf), "SealedLeaf.Name", "sealed class implements IName");
		Check (SealedExplicit (hidden), "SealedHidden.Hidden",
		       "sealed class implements IHidden explicitly");
		Check (SealedGeneric (generic), "SealedGen:7", "sealed class overrides Echo<T>");
		Check (SealedByRef (remote), "SealedRemote", "dispatch on a sealed marshal-by-ref class");
		Check (FromNewObj (), "Open", "newobj feeding a virtual call");
		Check (ThroughDelegate (op), "Op:6", "a delegate's Invoke");

		/* The open receiver, over both classes that can be in it. */
		Check (Dispatched (new Open ()), "Open", "dispatch on Open");
		Check (Dispatched (new OpenSub ()), "OpenSub", "dispatch on OpenSub");
		Check (Dispatched (new Middle ()), "Middle", "dispatch on Middle");

		return fails;
	}

	public static int Main ()
	{
		/* Enough entries to leave the interpreter and reach both compiled tiers. */
		for (int i = 0; i < 30000; ++i) {
			if (Run () != 0) {
				Console.WriteLine ("stopped at iteration {0}", i);
				return 1;
			}
		}

		/* The throwing arm, which is too slow to run in the loop above. A
		 * direct call has to raise where the dispatch would have. */
		CheckNullThrows (() => NullSealed (null), "null sealed receiver");
		CheckNullThrows (() => NullInterface (null), "null explicit-interface receiver");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
