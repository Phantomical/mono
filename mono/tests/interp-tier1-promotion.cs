using System;
using System.Runtime.CompilerServices;

/*
 * Every method here is called far more often than the promotion threshold, so
 * each one starts interpreted and is compiled part-way through its loop. What
 * that has to leave alone is the answers: the checks below run against both the
 * interpreted body and the compiled one that replaces it, and a tier switch
 * that lands wrong shows up as a value rather than as a crash.
 *
 * Run with --interp-tier0= so everything starts at tier 0. The same source
 * without the option is an ordinary all-compiled test, so a failure that is not
 * about promotion shows up in both.
 */

struct Small { public int a, b; }
struct Mixed { public int i; public double d; }
struct Big { public long a, b, c, d; }

interface ICounter { int Bump (int by); }

class Counter : ICounter {
	int n;
	[MethodImpl (MethodImplOptions.NoInlining)]
	public int Bump (int by) { n += by; return n; }
}

struct StructCounter : ICounter {
	int n;
	[MethodImpl (MethodImplOptions.NoInlining)]
	public int Bump (int by) { n += by; return n; }
}

class Base {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public virtual int Which (int x) { return x * 2; }
}

class Derived : Base {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public override int Which (int x) { return x * 3 + 1; }
}

class Gen<T> {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public int Count (T[] items) { return items.Length; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public T Pick (T[] items, int i) { return items [i]; }
}

static class Shapes {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Small MakeSmall (int x) { Small s; s.a = x; s.b = x + 1; return s; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int TakeSmall (Small s) { return s.a + s.b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Mixed MakeMixed (int x) { Mixed m; m.i = x; m.d = x * 0.5; return m; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int TakeMixed (Mixed m) { return m.i + (int) (m.d * 2); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Big MakeBig (int x) { Big b; b.a = x; b.b = x + 1; b.c = x + 2; b.d = x + 3; return b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long TakeBig (Big b) { return b.a + b.b + b.c + b.d; }

	/* More arguments than the interpreter's jit-call marshalling accepts, so
	 * this one keeps being interpreted by interpreted callers however well it
	 * is compiled. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int ManyArgs (int a, int b, int c, int d, int e, int f, int g, int h)
	{
		return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void RefOut (int x, ref int acc, out int doubled)
	{
		acc += x;
		doubled = x * 2;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Recurse (int n) { return n <= 1 ? 1 : n + Recurse (n - 1); }

	/* A frame that is promoted while it has clauses in flight is the case most
	 * likely to disagree with itself, so both a caught throw and a finally that
	 * has to run on the way out are here. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Throwing (int x)
	{
		int r = 0;

		try {
			if ((x & 3) == 0)
				throw new InvalidOperationException ("a");
			r += 1;
		} catch (InvalidOperationException) {
			r += 2;
		} finally {
			r += 4;
		}

		try {
			try {
				if ((x & 7) == 1)
					throw new ArgumentException ("b");
				r += 8;
			} finally {
				r += 16;
			}
		} catch (ArgumentException) {
			r += 32;
		}

		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Rethrown (int x)
	{
		try {
			try {
				if ((x & 1) == 0)
					throw new NotSupportedException ();
				return 1;
			} catch (NotSupportedException) {
				throw;
			}
		} catch (NotSupportedException) {
			return 2;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Strings (int x) { return ("v" + x).Length; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Arrays (int x)
	{
		int[] a = new int [8];

		for (int i = 0; i < a.Length; i++)
			a [i] = x + i;
		return a [x & 7];
	}
}

class Tier1Promotion {
	static int failures;

	static void Check (string what, int i, long got, long want)
	{
		if (got == want)
			return;

		/* One line per shape rather than per call: a shape that lands wrong
		 * after promotion is wrong for every later call too. */
		if (failures++ < 20)
			Console.WriteLine ("{0} at {1}: got {2}, want {3}", what, i, got, want);
	}

	public static int Main ()
	{
		Base b = new Base ();
		Base d = new Derived ();
		ICounter c = new Counter ();
		ICounter sc = new StructCounter ();
		Gen<string> gs = new Gen<string> ();
		Gen<int> gi = new Gen<int> ();
		string[] strs = { "a", "b", "c" };
		int[] ints = { 4, 5, 6, 7 };
		Func<int, int> del = Shapes.Recurse;
		int counted = 0, struct_counted = 0;

		/* Well past any threshold the runtime might be using, so each of these
		 * is interpreted at the top of the loop and compiled by the bottom. */
		for (int i = 0; i < 300; i++) {
			Check ("small", i, Shapes.TakeSmall (Shapes.MakeSmall (i)), 2 * i + 1);
			Check ("mixed", i, Shapes.TakeMixed (Shapes.MakeMixed (i)), 2 * i);
			Check ("big", i, Shapes.TakeBig (Shapes.MakeBig (i)), 4L * i + 6);
			Check ("manyargs", i,
			       Shapes.ManyArgs (i, i + 1, i + 2, i + 3, i + 4, i + 5, i + 6, i + 7),
			       36L * i + 168);

			int running = i, doubled;
			Shapes.RefOut (i, ref running, out doubled);
			Check ("refout", i, running + doubled, 4L * i);

			int n = (i & 15) + 1;
			Check ("recurse", i, Shapes.Recurse (n), (long) n * (n + 1) / 2);
			Check ("delegate", i, del (3), 6);

			long want_throwing = ((i & 3) == 0 ? 2 : 1) + 4
				+ ((i & 7) == 1 ? 32 : 8) + 16;
			Check ("throwing", i, Shapes.Throwing (i), want_throwing);
			Check ("rethrown", i, Shapes.Rethrown (i), (i & 1) == 0 ? 2 : 1);

			Check ("strings", i, Shapes.Strings (i), 1 + i.ToString ().Length);
			Check ("arrays", i, Shapes.Arrays (i), i + (i & 7));

			Check ("virtual-base", i, b.Which (i), 2L * i);
			Check ("virtual-derived", i, d.Which (i), 3L * i + 1);

			counted += i;
			Check ("interface-class", i, c.Bump (i), counted);
			/* Boxed, so each call bumps the same copy the box holds. */
			struct_counted += i;
			Check ("interface-struct", i, sc.Bump (i), struct_counted);

			Check ("generic-ref-count", i, gs.Count (strs), 3);
			Check ("generic-ref-pick", i, gs.Pick (strs, i % 3).Length, 1);
			Check ("generic-int-count", i, gi.Count (ints), 4);
			Check ("generic-int-pick", i, gi.Pick (ints, i % 4), 4 + i % 4);
		}

		if (failures != 0) {
			Console.WriteLine ("{0} failures", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
