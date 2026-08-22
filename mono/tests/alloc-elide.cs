using System;
using System.Runtime.CompilerServices;

/*
 * What an erased allocation must not change.
 *
 * The fast allocator is declared with allockind(alloc), which lets LLVM erase
 * an allocation that nothing uses. SROA leaves one of those behind when it
 * scalarizes a temporary object, and the arms below are the three answers that
 * has to give: erase the temporary, keep an object that something reads, and
 * keep an object whose fields are read straight out of fresh memory.
 *
 * ZeroStep pins that a field the constructor leaves alone reads as zero, and
 * KeptStep that a load at offset 0 still reads the vtable. An allocation
 * attribute describing the memory as zeroed or as uninitialized folds one of
 * those loads to a constant, and these two arms are what catches it.
 *
 * A finalizable class never reaches the fast allocator, so Finalized covers the
 * boundary from the other side.
 *
 * Each arm is one call of a small method, driven from a loop in Main. A method
 * holding its own loop is entered once, and both counters count entries, so the
 * arm would stay in the interpreter. The loop count gets the arms to tier 1.
 * Reaching tier 2 as well wants MONO_LLVM_JIT_TIER2_THRESHOLD lowered, because
 * these bodies are short enough that the program ends first.
 */

class Untouched {
	/* The constructor sets none of these, so every one must read as zero. */
	public int i;
	public long l;
	public double d;
	public object o;
	public IntPtr p;
}

class Escapes {
	public int v;
	public Escapes (int v) { this.v = v; }
}

class Finalized {
	public static volatile int collected;
	~Finalized () { collected++; }
}

class Vec {
	public double x, y, z;

	public Vec (double x, double y, double z) { this.x = x; this.y = y; this.z = z; }

	public static Vec Sub (Vec a, Vec b)
	{
		return new Vec (a.x - b.x, a.y - b.y, a.z - b.z);
	}
}

class AllocElide {
	const int N = 30000;

	static int failures;

	static void Check (string what, bool ok)
	{
		if (!ok) {
			Console.WriteLine ("FAILED: {0}", what);
			failures++;
		}
	}

	static long ZeroStep ()
	{
		Untouched u = new Untouched ();

		return u.i + u.l + (long) u.d + u.p.ToInt64 () + (u.o == null ? 0 : 1);
	}

	static int KeptStep (int i)
	{
		Escapes e = new Escapes (i);

		return (e.GetType () == typeof (Escapes) ? 1 : 0) + (e.v == i ? 1 : 0);
	}

	/*
	 * The fields are read here rather than in a second call. A callee the
	 * inliner declines takes the object as an argument, and it escapes.
	 */
	static double ElidedStep (Vec a, Vec b)
	{
		Vec v = Vec.Sub (a, b);

		return v.x * v.x + v.y * v.y + v.z * v.z;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void MakeGarbage ()
	{
		for (int i = 0; i < 2000; ++i)
			new Finalized ();
	}

	public static int Main ()
	{
		long zero = 0;
		int kept = 0;
		double dot = 0;
		Vec a = new Vec (3, 4, 5), b = new Vec (1, 2, 3);

		/* Enough entries to leave the interpreter and both compiled tiers. */
		for (int i = 0; i < N; ++i) {
			zero += ZeroStep ();
			kept += KeptStep (i);
			dot += ElidedStep (a, b);
		}

		Check ("untouched fields read as zero", zero == 0);
		Check ("a used object keeps its identity", kept == 2 * N);
		/* Sub gives (2,2,2), so the sum of squares is 12 each time. */
		Check ("scalarized arithmetic is unchanged", dot == 12.0 * N);

		MakeGarbage ();
		GC.Collect ();
		GC.WaitForPendingFinalizers ();
		GC.Collect ();
		GC.WaitForPendingFinalizers ();
		Check ("a finalizer still runs", Finalized.collected > 0);

		return failures == 0 ? 0 : 1;
	}
}
