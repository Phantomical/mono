// Exercises aliases that ManagedAA must not classify as NoAlias.

using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Base {
	public int x;
}

class Derived : Base {
	public int y;
}

interface ICounter {
	int Get ();
}

class Counter : ICounter {
	public int v;

	public int Get () { return v; }
}

class Pair {
	public Base b;
	public Derived d;
}

// The fields overlap to defeat type-based disambiguation.
[StructLayout (LayoutKind.Explicit)]
class Overlap {
	[FieldOffset (0)] public Counter c;
	[FieldOffset (0)] public Base b;
}

abstract class Shape {
	public abstract int Area ();
}

class Square : Shape {
	public int side = 2;

	public override int Area () { return side * side; }
}

static class Program {
	/* Promotion tiers accepted by PromoteNow. */
	const int tier1 = 3;
	const int tier2 = 4;

	static int[] sink = new int[64];
	static int[] spare = new int[64];
	static int count;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ThroughSubclass (Base b, Derived d, int n)
	{
		int t = 0;

		for (int i = 0; i < n; i++) {
			b.x = i;
			t += d.x;
		}
		return t;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Opaque ()
	{
	}

	// The ref store is untagged; object classes must distinguish it from the read.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ThroughFields (Pair p, int n)
	{
		ref int x = ref p.b.x;
		Opaque ();
		Derived d = p.d;
		int t = 0;

		if (d == null)
			throw new ArgumentNullException ();
		for (int i = 0; i < n; i++) {
			x = i;
			t += d.x;
		}
		return t;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ThroughElements (Base[] bs, Derived[] ds, int n)
	{
		ref int x = ref bs [0].x;
		Opaque ();
		Derived d = ds [0];
		int t = 0;

		if (d == null)
			throw new ArgumentNullException ();
		for (int i = 0; i < n; i++) {
			x = i;
			t += d.x;
		}
		return t;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ThroughOverlap (Base b, Overlap o, int n)
	{
		ref int x = ref b.x;
		Opaque ();
		Counter c = o.c;
		int t = 0;

		if (c == null)
			throw new ArgumentNullException ();
		for (int i = 0; i < n; i++) {
			x = i;
			t += c.v;
		}
		return t;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ThroughTwoArrayTypes (int[] a, uint[] b, int n)
	{
		int t = 0;

		for (int i = 0; i < n; i++) {
			a[0] = i;
			t += (int) b[0];
		}
		return t;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ThroughInterface (ICounter c, Counter k, int n)
	{
		int t = 0;

		for (int i = 0; i < n; i++) {
			k.v = i;
			t += c.Get ();
		}
		return t;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Swap ()
	{
		int[] held = sink;

		sink = spare;
		spare = held;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SwappedByCall (Shape s, int n)
	{
		for (int i = 0; i < n; i++) {
			sink [i & 63] += s.Area ();
			if (i == n / 2)
				Swap ();
		}
		return sink [0];
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SwappedByStore (Shape s, int n)
	{
		for (int i = 0; i < n; i++) {
			sink [i & 63] += s.Area ();
			if (i == n / 2)
				sink = spare;
		}
		return sink [0];
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int StaticBesideElements (int[] a, int n)
	{
		for (int i = 0; i < n; i++) {
			a [i & 63]++;
			count++;
		}
		return count;
	}

	static bool Promote (string name, int tier)
	{
		MethodInfo target = typeof (Program).GetMethod (name,
			BindingFlags.Static | BindingFlags.NonPublic);

		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: {0} () would not compile at tier {1}", name, tier);
		return false;
	}

	static bool Check (string name, int got, int wanted)
	{
		if (got == wanted)
			return true;

		Console.WriteLine ("FAIL: {0} got {1}, wanted {2}", name, got, wanted);
		return false;
	}

	public static int Main ()
	{
		string[] methods = {
			"ThroughSubclass", "ThroughTwoArrayTypes", "ThroughInterface",
			"SwappedByCall", "SwappedByStore", "StaticBesideElements",
			"ThroughFields", "ThroughElements", "ThroughOverlap",
		};

		const int n = 1000;

		// The receiver record is what devirtualizes c.Get (), which puts a read
		// of Counter.v through c into the loop.
		if (!Promote ("ThroughInterface", tier1))
			return 1;
		ThroughInterface (new Counter (), new Counter (), n);

		foreach (string name in methods)
			if (!Promote (name, tier2))
				return 1;

		int series = n * (n - 1) / 2;
		bool ok = true;

		Derived d = new Derived ();
		ok &= Check ("ThroughSubclass", ThroughSubclass (d, d, n), series);
		ok &= Check ("ThroughFields", ThroughFields (new Pair { b = d, d = d }, n), series);

		Derived[] ds = { d };
		ok &= Check ("ThroughElements", ThroughElements (ds, ds, n), series);

		Base shared = new Base ();
		ok &= Check ("ThroughOverlap", ThroughOverlap (shared, new Overlap { b = shared }, n), series);

		uint[] u = new uint [1];
		ok &= Check ("ThroughTwoArrayTypes", ThroughTwoArrayTypes ((int[]) (object) u, u, n), series);

		Counter k = new Counter ();
		ok &= Check ("ThroughInterface", ThroughInterface (k, k, n), series);

		// Element 0 takes turns 0, 64, ... 448 in the first array, which the
		// swap then retires, and 512, ... 960 in the second, at 4 a turn.
		Square square = new Square ();
		sink = new int [64];
		spare = new int [64];
		ok &= Check ("SwappedByCall", SwappedByCall (square, n), 4 * 8);

		sink = new int [64];
		spare = new int [64];
		ok &= Check ("SwappedByStore", SwappedByStore (square, n), 4 * 8);

		count = 0;
		ok &= Check ("StaticBesideElements", StaticBesideElements (new int [64], n), n);

		if (!ok)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
