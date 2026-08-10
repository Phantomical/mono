using System;
using System.Runtime.CompilerServices;

/*
 * Compiled code calling interpreted code, over every argument and return shape
 * the calling convention treats differently. Run with --interp-tier0=Callee so
 * that Callee's methods interpret while Main and Driver compile: every call
 * below then crosses from a compiled caller into the interpreter, through the
 * entry whose argument placement is the one thing neither side can be asked
 * about at run time.
 *
 * The same source run without the option is an ordinary all-compiled test, so
 * a failure that is not about the boundary shows up in both.
 */

struct Pair {
	public int a, b;
}

struct Mixed {
	public int i;
	public double d;
}

struct Big {
	public long a, b, c, d;
}

struct Tiny {
	public byte b;
}

/*
 * The seven bytes between b and l are a member of the layout the backend
 * generates, so they travel in a register of their own like any other field.
 */
struct Holed {
	public byte b;
	public long l;
}

struct TwoFloats {
	public float a, b;
}

struct FloatAndDouble {
	public float f;
	public double d;
}

struct IntAndFloat {
	public int i;
	public float f;
}

struct CalleeCounter {
	public int n;

	public int Bump (int by)
	{
		n += by;
		return n;
	}

	public override string ToString () { return "CalleeCounter:" + n; }
}

interface ICalleeCounter {
	int Bump (int by);
}

struct CalleeBoxed : ICalleeCounter {
	public int n;

	public int Bump (int by) { return n + by; }
}

class Callee {
	public int field = 7;

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Ints (sbyte a, short b, int c, long d)
	{
		return a + b + c + (int) d;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Unsigned (byte a, ushort b, uint c)
	{
		return a + b + (int) c;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static double Floats (float a, double b, float c, double d)
	{
		return a + b + c + d;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static double Mingled (int a, double b, int c, float d, long e)
	{
		return a + b + c + d + e;
	}

	/* Past the argument registers, so the tail of these lands on the stack. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long Spilled (long a, long b, long c, long d, long e, long f,
	                            long g, long h, long i, long j)
	{
		return a + b + c + d + e + f + g + h + i + j;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static double SpilledFloat (double a, double b, double c, double d,
	                                   double e, double f, double g, double h,
	                                   double i, double j)
	{
		return a + b + c + d + e + f + g + h + i + j;
	}

	/* Small value types travel in registers; a big one is copied to the stack. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int TakesPair (Pair p) { return p.a * 10 + p.b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static double TakesMixed (Mixed m) { return m.i + m.d; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long TakesBig (Big b) { return b.a + b.b + b.c + b.d; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int TakesTiny (Tiny t) { return t.b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int TakesBigAndInt (Big b, int i) { return (int) b.a + i; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long TakesHoled (Holed h) { return h.b + h.l; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static double TakesTwoFloats (TwoFloats f) { return f.a + f.b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static double TakesFloatAndDouble (FloatAndDouble f) { return f.f + f.d; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static double TakesIntAndFloat (IntAndFloat v) { return v.i + v.f; }

	/* The six longs use up the integer registers, so the pair behind them is
	 * read off the stack - one slot per field rather than an image of it. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long PastTheRegisters (long a, long b, long c, long d, long e,
	                                     long f, Pair p)
	{
		return a + b + c + d + e + f + p.a * 100 + p.b;
	}

	/* One register short, so the pair is split: its first field is the last
	 * register argument and its second is the first stack one. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long AcrossTheBoundary (long a, long b, long c, long d, long e,
	                                      Pair p)
	{
		return a + b + c + d + e + p.a * 100 + p.b;
	}

	/* Returns: in registers, and through the hidden pointer. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Pair MakesPair (int a, int b) { Pair p; p.a = a; p.b = b; return p; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Mixed MakesMixed (int i, double d) { Mixed m; m.i = i; m.d = d; return m; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Big MakesBig (long seed)
	{
		Big b;

		b.a = seed; b.b = seed + 1; b.c = seed + 2; b.d = seed + 3;
		return b;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Tiny MakesTiny (byte v) { Tiny t; t.b = v; return t; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Holed MakesHoled (byte b, long l) { Holed h; h.b = b; h.l = l; return h; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static TwoFloats MakesTwoFloats (float a, float b)
	{
		TwoFloats f; f.a = a; f.b = b; return f;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static FloatAndDouble MakesFloatAndDouble (float f, double d)
	{
		FloatAndDouble v; v.f = f; v.d = d; return v;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static IntAndFloat MakesIntAndFloat (int i, float f)
	{
		IntAndFloat v; v.i = i; v.f = f; return v;
	}

	/*
	 * The same prototype either way - one pointer in, one int out - and the two
	 * are read differently: a byref parameter is the pointer, an array
	 * reference is the slot holding one.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int ThroughRef (ref int a) { return a + 1; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int ThroughArray (int[] a) { return a[0] + 2; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void Void (int a, out int b) { b = a * 2; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void ByRef (ref Big b) { b.a += 100; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static string Str (string a, string b) { return a + b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static bool Bools (bool a, bool b) { return a & b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static char Chars (char a) { return char.ToUpperInvariant (a); }

	/* An instance method: the receiver is the argument the runtime insists on
	 * finding in the first register even when a big return needs one too. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public int Instance (int a) { return field + a; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public Big InstanceBig (long seed)
	{
		Big b;

		b.a = seed + field; b.b = 0; b.c = 0; b.d = 0;
		return b;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Throws (int a)
	{
		if (a > 0)
			throw new InvalidOperationException ("thrown at " + a);
		return a;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Generic<T> (T value, int n) { return value.ToString ().Length + n; }
}

class Driver {
	static int failures;

	static void Check (string what, long got, long want)
	{
		if (got != want) {
			Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
			failures++;
		}
	}

	static void Check (string what, double got, double want)
	{
		if (Math.Abs (got - want) > 1e-9) {
			Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
			failures++;
		}
	}

	static void Check (string what, string got, string want)
	{
		if (got != want) {
			Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
			failures++;
		}
	}

	public static int Main ()
	{
		Check ("Ints", Callee.Ints (-1, 2, 3, 4), 8);
		Check ("Unsigned", Callee.Unsigned (1, 2, 3), 6);
		Check ("Floats", Callee.Floats (1.5f, 2.25, 3.5f, 4.25), 11.5);
		Check ("Mingled", Callee.Mingled (1, 2.5, 3, 4.5f, 5), 16.0);
		Check ("Spilled", Callee.Spilled (1, 2, 3, 4, 5, 6, 7, 8, 9, 10), 55);
		Check ("SpilledFloat",
		       Callee.SpilledFloat (1, 2, 3, 4, 5, 6, 7, 8, 9, 10), 55.0);

		Pair p; p.a = 4; p.b = 2;
		Check ("TakesPair", Callee.TakesPair (p), 42);

		Mixed m; m.i = 3; m.d = 0.5;
		Check ("TakesMixed", Callee.TakesMixed (m), 3.5);

		Big big = Callee.MakesBig (10);
		Check ("MakesBig", big.a + big.b + big.c + big.d, 46);
		Check ("TakesBig", Callee.TakesBig (big), 46);
		Check ("TakesBigAndInt", Callee.TakesBigAndInt (big, 5), 15);

		Tiny t = Callee.MakesTiny (200);
		Check ("MakesTiny", t.b, 200);
		Check ("TakesTiny", Callee.TakesTiny (t), 200);

		Holed holed = Callee.MakesHoled (3, 40);
		Check ("MakesHoled", holed.b * 1000 + holed.l, 3040);
		Check ("TakesHoled", Callee.TakesHoled (holed), 43);

		TwoFloats twof = Callee.MakesTwoFloats (1.5f, 2.25f);
		Check ("MakesTwoFloats", twof.a + twof.b, 3.75);
		Check ("TakesTwoFloats", Callee.TakesTwoFloats (twof), 3.75);

		FloatAndDouble fd = Callee.MakesFloatAndDouble (0.5f, 4.25);
		Check ("MakesFloatAndDouble", fd.f + fd.d, 4.75);
		Check ("TakesFloatAndDouble", Callee.TakesFloatAndDouble (fd), 4.75);

		IntAndFloat inf = Callee.MakesIntAndFloat (6, 0.25f);
		Check ("MakesIntAndFloat", inf.i + inf.f, 6.25);
		Check ("TakesIntAndFloat", Callee.TakesIntAndFloat (inf), 6.25);

		Pair edge; edge.a = 7; edge.b = 9;
		Check ("PastTheRegisters",
		       Callee.PastTheRegisters (1, 2, 3, 4, 5, 6, edge), 730);
		Check ("AcrossTheBoundary",
		       Callee.AcrossTheBoundary (1, 2, 3, 4, 5, edge), 724);

		int through = 5;
		Check ("ThroughRef", Callee.ThroughRef (ref through), 6);
		Check ("ThroughArray", Callee.ThroughArray (new int[] { 5 }), 7);

		Pair made = Callee.MakesPair (7, 8);
		Check ("MakesPair", made.a * 100 + made.b, 708);

		Mixed madeMixed = Callee.MakesMixed (9, 1.25);
		Check ("MakesMixed", madeMixed.i + madeMixed.d, 10.25);

		int outv;
		Callee.Void (21, out outv);
		Check ("Void", outv, 42);

		Callee.ByRef (ref big);
		Check ("ByRef", big.a, 110);

		Check ("Str", Callee.Str ("ab", "cd"), "abcd");
		Check ("Bools", Callee.Bools (true, true) ? 1 : 0, 1);
		Check ("Chars", Callee.Chars ('q'), 'Q');

		Callee c = new Callee ();
		Check ("Instance", c.Instance (5), 12);
		Check ("InstanceBig", c.InstanceBig (1).a, 8);

		try {
			Callee.Throws (3);
			Check ("Throws", 0, 1);
		} catch (InvalidOperationException e) {
			Check ("Throws", e.Message, "thrown at 3");
		}

		Check ("Generic<int>", Callee.Generic<int> (1234, 1), 5);
		Check ("Generic<string>", Callee.Generic<string> ("abcdef", 2), 8);

		/* Value-type instance methods: the boxed receiver arrives through the
		 * unboxing entry when the call comes off a vtable or an interface. */
		CalleeCounter counter = new CalleeCounter ();
		counter.n = 1;
		Check ("struct direct", counter.Bump (2), 3);
		Check ("struct virtual", counter.ToString (), "CalleeCounter:3");

		CalleeBoxed b = new CalleeBoxed ();
		b.n = 10;
		ICalleeCounter ic = b;
		Check ("struct interface", ic.Bump (5), 15);

		object boxed = counter;
		Check ("struct boxed tostring", boxed.ToString (), "CalleeCounter:3");

		if (failures != 0) {
			Console.WriteLine ("{0} failures", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
