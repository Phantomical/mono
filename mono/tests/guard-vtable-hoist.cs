// A guarded dispatch in a loop whose receiver does not change, at tier 2, where
// the guard's vtable read moves in front of the loop. What the loop does has to
// stay the same for a receiver the guard misses on, for a null receiver, and for
// a proxy whose vtable a cast inside the loop replaces.

using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.Remoting.Messaging;
using System.Runtime.Remoting.Proxies;

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

interface IArea {
	int Area ();
}

interface IOther {
}

abstract class Shape : IArea {
	public abstract int Area ();
}

class Square : Shape {
	public override int Area () { return 4; }
}

class Circle : Shape {
	public override int Area () { return 3; }
}

class Target : MarshalByRefObject, IArea, IOther {
	public int Area () { return 9; }
}

class MyProxy : RealProxy, System.Runtime.Remoting.IRemotingTypeInfo {
	object target;

	public MyProxy (object target) : base (typeof (MarshalByRefObject))
	{
		this.target = target;
	}

	public string TypeName { get { return "MyProxy"; } set { } }

	public bool CanCastTo (Type t, object o) { return true; }

	public override IMessage Invoke (IMessage msg)
	{
		IMethodCallMessage call = (IMethodCallMessage) msg;
		object result = call.MethodBase.Invoke (target, call.Args);

		return new ReturnMessage (result, null, 0, null, call);
	}
}

class GuardVtableHoist {
	// MonoTier::tier1 and MonoTier::tier2, as PromoteNow takes them.
	const int tier1 = 3;
	const int tier2 = 4;

	static int failures;

	static void Check (string what, int got, int want)
	{
		if (got != want) {
			Console.WriteLine ("{0}: got {1}, want {2}", what, got, want);
			failures++;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Store (Shape s, int[] sink, int n)
	{
		int t = 0;

		for (int i = 0; i < n; i++) {
			sink[i & 7]++;
			t += s.Area ();
		}
		return t;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool Widen (IArea a)
	{
		return a is IOther;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Spin (IArea a, int n)
	{
		int t = 0;

		for (int i = 0; i < n; i++) {
			t += a.Area ();
			if (Widen (a))
				t += 100;
		}
		return t;
	}

	static bool Promote (string name, int tier)
	{
		MethodInfo m = typeof (GuardVtableHoist).GetMethod (
			name, BindingFlags.Static | BindingFlags.NonPublic);

		return Mono.Tiering.MonoTier.PromoteNow (m.MethodHandle.Value, tier);
	}

	static int Main ()
	{
		string[] roots = { "Store", "Spin" };

		foreach (string root in roots) {
			if (!Promote (root, tier1)) {
				Console.WriteLine ("{0}: would not compile at tier 1", root);
				return 1;
			}
		}

		Shape square = new Square ();
		IArea real = new Square ();
		int[] sink = new int[8];

		for (int i = 0; i < 100; i++) {
			Store (square, sink, 4);
			Spin (real, 4);
		}

		foreach (string root in roots) {
			if (!Promote (root, tier2)) {
				Console.WriteLine ("{0}: would not compile at tier 2", root);
				return 1;
			}
		}

		Check ("Store Square", Store (square, new int[8], 10), 40);
		Check ("Store Circle", Store (new Circle (), new int[8], 10), 30);

		// The null check on the receiver follows the store, so the first turn's
		// store lands before the exception.
		int[] before = new int[8];

		try {
			Store (null, before, 10);
			Console.WriteLine ("Store null: did not throw");
			failures++;
		} catch (NullReferenceException) {
			Check ("Store null, stores", before[0] + before[1], 1);
		}

		// The cast in Spin () gives the proxy IOther on the first turn, which
		// replaces its vtable in the middle of the loop.
		IArea proxy = (IArea) new MyProxy (new Target ()).GetTransparentProxy ();

		Check ("Spin Square", Spin (real, 3), 12);
		Check ("Spin proxy", Spin (proxy, 3), 327);

		if (failures != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
