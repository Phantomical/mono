using System;
using System.Runtime.CompilerServices;
using System.Threading;

//
// Static-field access at tier 1.
//
// A class's static fields share one runtime allocation, and the tier-1 backend
// names that block as a single linker symbol and reaches each field as a
// constant offset into it. A field's address is therefore (block, offset)
// rather than a baked absolute, and either half can be wrong on its own:
//
//   - a bad offset reads a neighbouring field of the SAME class, so every field
//     here holds a distinct value, and the widths are mixed so that a slip of a
//     few bytes lands somewhere observable rather than on an identical int;
//   - a bad block reads the right offset in the WRONG class, so Twin duplicates
//     Home's layout exactly, and Gen<T> duplicates it once per instantiation -
//     the case where "one symbol per class" has to mean one per instantiation;
//   - overstated alignment would let LLVM pick an aligned access for a field
//     that is not, so the block deliberately contains sub-word fields.
//
// The special statics are the exception - thread- and context-statics live in
// per-domain side tables rather than in the block, and so take a different path
// - which is why Home carries a [ThreadStatic] field next to its ordinary ones.
//
// Every test is correct whatever tier it runs at, so this passes on the classic
// JIT too; under tiering it is the encoding that is on trial.
//
//   MONO_PATH=<class> MONO_TIERED=1 MONO_TIERED_CALL_THRESHOLD=0 \
//     mono --llvm tiered-statics.exe
//

struct Pair {
	public int X;
	public long Y;
}

class Home {
	public static byte B;
	public static short S;
	public static int I;
	public static long L;
	public static string Str;
	public static object Obj;
	public static Pair P;

	[ThreadStatic] public static int T;
	[ThreadStatic] public static long TL;

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void Fill (int seed)
	{
		B = (byte) (seed + 1);
		S = (short) (seed + 2);
		I = seed + 3;
		L = seed + 4;
		Str = "home";
		Obj = null;
		P.X = seed + 5;
		P.Y = seed + 6;
	}

	// Summed with distinct weights: two fields swapped by a bad offset changes
	// the total, which an unweighted sum would hide.
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long Sum ()
	{
		return B * 1L + S * 10L + I * 100L + L * 1000L + P.X * 10000L + P.Y * 100000L;
	}
}

// Home's layout, field for field, so that a block mixed up between the two
// still type-checks and still reads plausible values - only the numbers differ.
class Twin {
	public static byte B;
	public static short S;
	public static int I;
	public static long L;
	public static string Str;
	public static object Obj;
	public static Pair P;

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void Fill (int seed)
	{
		B = (byte) (seed + 1);
		S = (short) (seed + 2);
		I = seed + 3;
		L = seed + 4;
		Str = "twin";
		P.X = seed + 5;
		P.Y = seed + 6;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long Sum ()
	{
		return B * 1L + S * 10L + I * 100L + L * 1000L + P.X * 10000L + P.Y * 100000L;
	}
}

class Gen<T> {
	public static byte B;
	public static short S;
	public static int I;
	public static long L;
	public static Pair P;

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void Fill (int seed)
	{
		B = (byte) (seed + 1);
		S = (short) (seed + 2);
		I = seed + 3;
		L = seed + 4;
		P.X = seed + 5;
		P.Y = seed + 6;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static long Sum ()
	{
		return B * 1L + S * 10L + I * 100L + L * 1000L + P.X * 10000L + P.Y * 100000L;
	}
}

// A class whose statics are only ever reached by address (ldsflda), which is
// the patch used unmodified rather than as the base of a load.
class ByRef {
	public static int A;
	public static int B;
	public static long C;

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static ref int RefA () { return ref A; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static ref int RefB () { return ref B; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static ref long RefC () { return ref C; }
}

class Boxes {
	public static object A;
	public static object B;
	public static string C;

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void Store (object a, object b, string c) { A = a; B = b; C = c; }
}

public class TieredStatics {
	// Enough calls to cross the highest threshold the suite sweeps (1000), so
	// the helpers really are running at tier 1 by the time the checks matter.
	const int Hot = 2500;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long Expected (int seed)
	{
		return (byte) (seed + 1) * 1L + (short) (seed + 2) * 10L + (seed + 3) * 100L +
			(seed + 4) * 1000L + (seed + 5) * 10000L + (seed + 6) * 100000L;
	}

	// Every field of one class, at mixed widths. A wrong offset within the
	// block lands on a neighbour and changes the weighted sum.
	public static int test_0_fields_within_a_block ()
	{
		for (int i = 0; i < Hot; ++i) {
			int seed = i & 63;
			Home.Fill (seed);
			if (Home.Sum () != Expected (seed))
				return 1;
		}
		if (Home.Str != "home")
			return 2;
		return 0;
	}

	// Two classes laid out identically, written alternately: a block confused
	// for the other one reads the other's seed.
	public static int test_0_blocks_are_per_class ()
	{
		for (int i = 0; i < Hot; ++i) {
			int a = i & 31, b = (i & 31) + 40;
			Home.Fill (a);
			Twin.Fill (b);
			if (Home.Sum () != Expected (a))
				return 1;
			if (Twin.Sum () != Expected (b))
				return 2;
			// Re-read after the other class was written, which is where a
			// shared block shows up rather than at the first read.
			Twin.Fill (b + 1);
			if (Home.Sum () != Expected (a))
				return 3;
		}
		if (Home.Str != "home" || Twin.Str != "twin")
			return 4;
		return 0;
	}

	// The same again one level down: each instantiation of a generic has its
	// own block, so "one symbol per class" must not collapse them.
	public static int test_0_blocks_are_per_instantiation ()
	{
		for (int i = 0; i < Hot; ++i) {
			int a = i & 15, b = (i & 15) + 20, c = (i & 15) + 50;
			Gen<int>.Fill (a);
			Gen<string>.Fill (b);
			Gen<object>.Fill (c);
			if (Gen<int>.Sum () != Expected (a))
				return 1;
			if (Gen<string>.Sum () != Expected (b))
				return 2;
			if (Gen<object>.Sum () != Expected (c))
				return 3;
		}
		return 0;
	}

	// Thread statics are NOT in the block; ordinary statics of the same class
	// are. Both are touched here, and a second thread proves the thread-static
	// half still resolves per thread rather than to one shared slot.
	public static int test_0_special_statics ()
	{
		Home.T = 0;
		Home.TL = 0;
		for (int i = 0; i < Hot; ++i) {
			Home.T += 1;
			Home.TL += 2;
			Home.I = i;
		}
		if (Home.T != Hot || Home.TL != 2L * Hot)
			return 1;

		int other = -1;
		long otherL = -1;
		var th = new Thread (() => {
			for (int i = 0; i < Hot; ++i) {
				Home.T += 3;
				Home.TL += 4;
			}
			other = Home.T;
			otherL = Home.TL;
		});
		th.Start ();
		th.Join ();

		// The new thread starts from zero, and the first thread is untouched.
		if (other != 3 * Hot || otherL != 4L * Hot)
			return 2;
		if (Home.T != Hot || Home.TL != 2L * Hot)
			return 3;
		return 0;
	}

	// Taking a static's address rather than loading through it: the patch value
	// is the result here, so a bad offset is not masked by a following load.
	public static int test_0_static_addresses ()
	{
		for (int i = 0; i < Hot; ++i) {
			ref int a = ref ByRef.RefA ();
			ref int b = ref ByRef.RefB ();
			ref long c = ref ByRef.RefC ();
			a = i;
			b = i + 1;
			c = i + 2;
			if (ByRef.A != i || ByRef.B != i + 1 || ByRef.C != i + 2)
				return 1;
			// Distinct fields must be distinct storage: writing through one
			// reference must not disturb the other.
			a = 7;
			if (ByRef.B != i + 1 || ByRef.C != i + 2)
				return 2;
		}
		return 0;
	}

	// Reference-typed statics go through a write barrier, which card-marks the
	// address the store used - so a wrong address is a wrong card. Collect
	// between rounds so a missed or misplaced barrier loses the object.
	public static int test_0_reference_statics ()
	{
		for (int i = 0; i < Hot / 25; ++i) {
			Boxes.Store (new int[4] { i, i + 1, i + 2, i + 3 },
			             new object (),
			             string.Concat ("s", i.ToString ()));
			GC.Collect ();
			GC.WaitForPendingFinalizers ();

			int[] a = Boxes.A as int[];
			if (a == null || a.Length != 4 || a [0] != i || a [3] != i + 3)
				return 1;
			if (Boxes.B == null)
				return 2;
			if (Boxes.C != "s" + i.ToString ())
				return 3;
		}
		return 0;
	}

	public static int Main (string[] args)
	{
		return TestDriver.RunTests (typeof (TieredStatics), args);
	}
}
