using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Interpreted code calling compiled code, over the argument and return shapes
 * the marshalling between them treats differently. Run with
 * --interp-tier0=Interpreted so that Interpreted's methods run in the
 * interpreter while everything else compiles: Warm () gives every callee code
 * before Run () is entered, so each call Run () makes leaves the interpreter
 * rather than staying in it.
 *
 * Shapes the marshalling refuses - more than six parameters, a generic
 * instance - are here too, and have to keep their values by being interpreted.
 *
 * The same source run without the option is an ordinary all-compiled test, so a
 * failure that is not about the boundary shows up in both.
 */

struct Pair {
	public int a, b;

	[MethodImpl (MethodImplOptions.NoInlining)]
	public Pair (int a, int b) { this.a = a; this.b = b; }
}

struct Mixed {
	public int i;
	public double d;
}

struct Big {
	public long a, b, c, d;
}

interface ICounter {
	int Bump (int by);
}

struct BoxedCounter : ICounter {
	public int n;

	[MethodImpl (MethodImplOptions.NoInlining)]
	public int Bump (int by) { n += by; return n; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public override string ToString () { return "BoxedCounter:" + n; }
}

class Base {
	public int field;

	[MethodImpl (MethodImplOptions.NoInlining)]
	public Base (int field) { this.field = field; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public virtual int Virt (int a) { return field + a; }
}

class Derived : Base {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public Derived (int field) : base (field) { }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public override int Virt (int a) { return field * a; }
}

delegate int Adder (int a, int b);

class Compiled {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Ints (sbyte a, short b, int c, long d) { return a + b + c + (int) d; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static byte SmallUnsigned (int a) { return (byte) a; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static short SmallSigned (int a) { return (short) a; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static double Floats (float a, double b, float c) { return a + b + c; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Six (int a, int b, int c, int d, int e, int f) { return a + b + c + d + e + f; }

	/* One over what the marshalling will take, so this one stays interpreted. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Seven (int a, int b, int c, int d, int e, int f, int g)
	{
		return a + b + c + d + e + f + g;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Two (int a, int b) { return a + b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int TakesPair (Pair p) { return p.a * 10 + p.b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Pair MakesPair (int a, int b) { Pair p; p.a = a; p.b = b; return p; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Mixed MakesMixed (int i, double d) { Mixed m; m.i = i; m.d = d; return m; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Big MakesBig (long seed) { Big b; b.a = seed; b.b = 2; b.c = 3; b.d = 4; return b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long TakesBig (Big b) { return b.a + b.b + b.c + b.d; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void Out (int a, out int b) { b = a * 2; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void ByRef (ref Big b) { b.a += 100; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static string Str (string a, string b) { return a + b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static bool Bool (int a) { return a > 0; }

	/* Inflated, so this one stays interpreted too. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Generic<T> (T value, int n) { return value.ToString ().Length + n; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Throws (int a) { throw new InvalidOperationException ("thrown at " + a); }

	/* Compiled, and calls straight back into the interpreter. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int RoundTrip (int a) { return Interpreted.Helper (a) + 1; }
}

class Late {
	/* Nothing has code for this one when Run () first calls it: the reflected
	 * call part way through the loop below is what gives it some. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Later (int a) { return a + 11; }
}

class Interpreted {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Helper (int a) { return a * 3; }

	public static int Run ()
	{
		Driver.Check ("Ints", Compiled.Ints (-1, 2, 3, 4), 8);
		Driver.Check ("SmallUnsigned", Compiled.SmallUnsigned (-1), 255);
		Driver.Check ("SmallSigned", Compiled.SmallSigned (0xffff), -1);
		Driver.Check ("Floats", Compiled.Floats (1.5f, 2.25, 3.5f), 7.25);
		Driver.Check ("Six", Compiled.Six (1, 2, 3, 4, 5, 6), 21);
		Driver.Check ("Seven", Compiled.Seven (1, 2, 3, 4, 5, 6, 7), 28);

		Pair p; p.a = 4; p.b = 2;
		Driver.Check ("TakesPair", Compiled.TakesPair (p), 42);

		Pair made = Compiled.MakesPair (7, 8);
		Driver.Check ("MakesPair", made.a * 100 + made.b, 708);

		Mixed m = Compiled.MakesMixed (9, 1.25);
		Driver.Check ("MakesMixed", m.i + m.d, 10.25);

		Big big = Compiled.MakesBig (10);
		Driver.Check ("MakesBig", Compiled.TakesBig (big), 19);

		Compiled.ByRef (ref big);
		Driver.Check ("ByRef", big.a, 110);

		int outv;
		Compiled.Out (21, out outv);
		Driver.Check ("Out", outv, 42);

		Driver.Check ("Str", Compiled.Str ("ab", "cd"), "abcd");
		Driver.Check ("Bool", Compiled.Bool (1) ? 1 : 0, 1);
		Driver.Check ("Generic", Compiled.Generic<int> (1234, 5), 9);
		Driver.Check ("RoundTrip", Compiled.RoundTrip (5), 16);

		/* A constructor is reached through the same call path. */
		Driver.Check ("Ctor", new Base (3).field, 3);
		Driver.Check ("VtCtor", new Pair (5, 6).a * 10 + new Pair (5, 6).b, 56);

		/* Virtual dispatch resolves the implementation before the call is made. */
		Base b = new Derived (6);
		Driver.Check ("Virt derived", b.Virt (7), 42);
		b = new Base (6);
		Driver.Check ("Virt base", b.Virt (7), 13);

		/* A boxed value type reached through an interface: the receiver has to
		 * be stepped past the object header first. */
		ICounter ic = new BoxedCounter ();
		Driver.Check ("Boxed bump", ic.Bump (4), 4);
		Driver.Check ("Boxed bump again", ic.Bump (4), 8);
		Driver.Check ("Boxed ToString", ic.ToString (), "BoxedCounter:8");

		Adder add = Compiled.Two;
		Driver.Check ("Delegate", add (3, 4), 7);

		/* An exception thrown by the callee, caught here. */
		try {
			Compiled.Throws (9);
			Driver.Check ("Throws returned", 1, 0);
		} catch (InvalidOperationException e) {
			Driver.Check ("Throws message", e.Message, "thrown at 9");
		}

		/* And one that only the caller's finally sees on the way past. */
		int ran = 0;
		try {
			try {
				Compiled.Throws (10);
			} finally {
				ran = 1;
			}
		} catch (InvalidOperationException) {
		}
		Driver.Check ("Finally ran", ran, 1);

		/* Later () has no code for the first half of this loop and gets some
		 * part way through, so the call site has to notice on its own. */
		int late = 0;
		for (int i = 0; i < 20; i++) {
			if (i == 10)
				typeof (Late).GetMethod ("Later").Invoke (null, new object [] { 0 });
			late += Late.Later (i);
		}
		Driver.Check ("Late", late, 410);

		return Driver.failures;
	}
}

class Driver {
	public static int failures;

	public static void Check (string what, long got, long want)
	{
		if (got != want) {
			Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
			failures++;
		}
	}

	public static void Check (string what, double got, double want)
	{
		if (Math.Abs (got - want) > 1e-9) {
			Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
			failures++;
		}
	}

	public static void Check (string what, string got, string want)
	{
		if (got != want) {
			Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
			failures++;
		}
	}

	/*
	 * Every callee Run () uses, called once from here so that it has code by
	 * the time the interpreter reaches it. Nothing in the interpreter asks for
	 * a method to be compiled, so without this the whole test would interpret.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Warm ()
	{
		Big big = Compiled.MakesBig (1);
		int outv;

		Compiled.Ints (1, 1, 1, 1);
		Compiled.SmallUnsigned (1);
		Compiled.SmallSigned (1);
		Compiled.Floats (1, 1, 1);
		Compiled.Six (1, 1, 1, 1, 1, 1);
		Compiled.Seven (1, 1, 1, 1, 1, 1, 1);
		Compiled.TakesPair (new Pair (1, 1));
		Compiled.MakesPair (1, 1);
		Compiled.MakesMixed (1, 1);
		Compiled.TakesBig (big);
		Compiled.ByRef (ref big);
		Compiled.Out (1, out outv);
		Compiled.Str ("a", "b");
		Compiled.Bool (1);
		Compiled.Generic<int> (1, 1);
		Compiled.RoundTrip (1);
		Compiled.Two (1, 1);

		new Derived (1).Virt (1);
		new Base (1).Virt (1);

		ICounter ic = new BoxedCounter ();
		ic.Bump (1);
		ic.ToString ();

		try { Compiled.Throws (1); } catch (InvalidOperationException) { }
	}

	public static int Main ()
	{
		Warm ();
		return Interpreted.Run () == 0 ? 0 : 1;
	}
}
