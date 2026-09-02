using System;
using System.Runtime.CompilerServices;

/*
 * What an erased allocation must not change.
 *
 * `mono.alloc.object` and `mono.alloc.vector` carry allockind(alloc), which
 * lets LLVM erase an allocation that nothing uses. SROA leaves one of those
 * behind when it scalarizes a temporary object, and the arms below are the
 * three answers that has to give: erase the temporary, keep an object that
 * something reads, and keep an object whose fields are read straight out of
 * fresh memory.
 *
 * The same two declarations carry allockind(zeroed), so a load of a word nothing
 * wrote answers zero without reading memory. ZeroStep reads such a word, and
 * FreshElementStep reads one twice: once where the read can fold, and once
 * through a call where it cannot.
 *
 * The words an allocator writes itself are stored again beside the call.
 * KeptStep and LengthStep catch either store going missing: a fresh object then
 * reads a null vtable, and a fresh array a length of zero.
 *
 * A finalizable class allocates through `mono.alloc.object.kept`, which carries
 * no alloc kind, so Finalized covers the boundary from the other side. The
 * `runtime-alloc-kept` arm runs the whole program with sequence points on,
 * where every class takes that form.
 *
 * StraddleStep covers what the declaration claims about memory rather than the
 * erasure. Under `memory(argmem: read, inaccessiblemem: readwrite)` a load
 * below an allocation reads the store above it, so the arm answers wrong where
 * an allocation writes a field its caller named.
 *
 * Both collectors run every arm against the same declaration, so an arm that
 * answers differently under the two names the collector.
 *
 * `mono.alloc.string` is the third declaration, reached through `ToUpper ()`
 * rather than through IL this file writes: FastAllocateString is internal to
 * corlib. It carries `allockind(uninitialized)` rather than `(zeroed)`, since
 * `mono_gc_alloc_string ()` leaves the characters unset under Boehm.
 * KeptStringStep is KeptArrayStep's arm for it: the length store and the
 * characters both have to survive whichever tier compiles the call.
 *
 * Each arm is one call of a small method, driven from a loop in Main. A method
 * holding its own loop is entered once, and both counters count entries, so the
 * arm would stay in the interpreter. The loop count gets the arms to tier 1.
 * Reaching tier 2 as well wants the tier-2 threshold lowered, because these
 * bodies are short enough that the program ends first. The
 * `runtime-alloc-zeroed` arm lowers it, which is what reaches GVN and DSE.
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

class Cell {
	public int value;
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

	/*
	 * An array carries the same attribute, so the arms below are the array side
	 * of the two answers above. A dead array goes, and one that is read stays.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int DeadArrayStep (int n)
	{
		int[] unused = new int[n & 7];

		return n + 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int KeptArrayStep (int n)
	{
		int[] a = new int[4];

		a[n & 3] = n;
		return (a[n & 3] == n ? 1 : 0) + (a.Length == 4 ? 1 : 0);
	}

	/*
	 * The length is no constant, so what the read answers is the store
	 * emit_vector_alloc () writes behind the call. Without it the read folds to
	 * zero.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int LengthStep (int n)
	{
		int[] a = new int[n & 7];

		return a.Length;
	}

	/*
	 * The length varies with n, so what Length answers is the store
	 * emit_string_alloc () writes behind the call. 'A' is what
	 * ToUpperInternal () wrote into memory FastAllocateString left
	 * uninitialized.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int KeptStringStep (int n)
	{
		string s = new string ('a', (n & 7) + 1).ToUpper ();

		return s.Length + (s[0] == 'A' ? 1 : 0);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadsElement (int[] a, int at)
	{
		return a[at];
	}

	/*
	 * The read here can fold from the allocation and the one in ReadsElement
	 * cannot, so the two disagree where a block comes back dirty. Both indexes
	 * are constants, so the store above does not clobber the read.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FreshElementStep (int n)
	{
		int[] a = new int[8];

		a[0] = n;
		return a[1] - ReadsElement (a, 1);
	}

	/*
	 * A negative length raises even where the array itself is dead. This is the
	 * arm that regressed when arrays first took the attribute: the raise lived
	 * inside the allocator, so erasing the call took the exception with it.
	 *
	 * Nothing may read the array. One read keeps the allocation alive, the
	 * allocator raises as it always did, and the arm stops testing anything.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool RefusesNegativeDead (int n)
	{
		try {
			int[] unused = new int[n];

			return false;
		} catch (OverflowException) {
			return true;
		}
	}

	/*
	 * The store and the load straddle an allocation that stands, because the
	 * length is read. The field is an int, so no write barrier sits between
	 * them and this arm answers on the allocation alone.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int StraddleStep (Cell c, int n)
	{
		c.value = n;

		int[] a = new int[4];

		return c.value + a.Length;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool RefusesNegativeRead (int n)
	{
		try {
			return new int[n].Length < 0;
		} catch (OverflowException) {
			return true;
		}
	}

	public static int Main ()
	{
		long zero = 0;
		int kept = 0;
		double dot = 0;
		int dead = 0, keptArray = 0, refusedDead = 0, refusedRead = 0;
		int length = 0, expectedLength = 0, freshElement = 0, keptString = 0;
		long straddle = 0;
		Vec a = new Vec (3, 4, 5), b = new Vec (1, 2, 3);
		Cell cell = new Cell ();

		/* Enough entries to leave the interpreter and both compiled tiers. */
		for (int i = 0; i < N; ++i) {
			zero += ZeroStep ();
			kept += KeptStep (i);
			dot += ElidedStep (a, b);
			dead += DeadArrayStep (i);
			keptArray += KeptArrayStep (i);
			length += LengthStep (i);
			expectedLength += i & 7;
			keptString += KeptStringStep (i);
			freshElement += FreshElementStep (i);
			straddle += StraddleStep (cell, i);

			if (RefusesNegativeDead (-1))
				refusedDead++;
			if (RefusesNegativeRead (-1))
				refusedRead++;
		}

		Check ("untouched fields read as zero", zero == 0);
		Check ("a used object keeps its identity", kept == 2 * N);
		/* Sub gives (2,2,2), so the sum of squares is 12 each time. */
		Check ("scalarized arithmetic is unchanged", dot == 12.0 * N);

		/* The sum of i + 1 over the loop, which the erased array cannot change. */
		Check ("a dead array changes no answer", dead == (long) N * (N + 1) / 2);
		Check ("a used array keeps its elements", keptArray == 2 * N);
		Check ("a fresh array reads back its length", length == expectedLength);
		Check ("a fresh string reads back its length and its characters",
		       keptString == expectedLength + 2 * N);
		Check ("an element nothing wrote reads as zero", freshElement == 0);
		Check ("a dead array's negative length still raises", refusedDead == N);
		Check ("a read array's negative length still raises", refusedRead == N);

		/* Each step answers i + 4, so the sum is the sum of i plus 4 for each. */
		Check ("a field read below an allocation reads the store above it",
		       straddle == (long) N * (N - 1) / 2 + 4 * (long) N);

		MakeGarbage ();
		GC.Collect ();
		GC.WaitForPendingFinalizers ();
		GC.Collect ();
		GC.WaitForPendingFinalizers ();
		Check ("a finalizer still runs", Finalized.collected > 0);

		return failures == 0 ? 0 : 1;
	}
}
