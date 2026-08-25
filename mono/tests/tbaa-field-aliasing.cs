/*
 * Accesses that reach one slot through two receivers the JIT cannot tell apart.
 *
 * Most cases below hold two references that are the same object at run time. A
 * store goes through one and a load through the other. The two addresses come
 * from unrelated values, so LLVM's address-based analysis cannot connect them,
 * and the `!tbaa` tag is the only thing that keeps the load and the store
 * together. A tag that is too fine turns such a case into a dropped store.
 *
 * Two receivers are what gives those cases teeth. With one, LLVM sees one
 * pointer and answers from the address before it reads the tags, and the case
 * then passes under a tree that is wrong. Keep the pairs where they are there.
 *
 * mono/tests/tbaa-aliasing.cs holds the other half: one slot under two types,
 * which is what the reference and scalar split has to survive.
 *
 * Every case runs at all three tiers, the way tbaa-aliasing.cs does.
 */

using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

/// Four scalars in one value type, which is the shape a per-field tag exists
/// for.
struct Vec4 {
	public double A;
	public double B;
	public double C;
	public double D;
}

class Counter {
	public int Value;
	public int Other;
}

class Derived : Counter {
	public int Own;
}

class Box<T> {
	public int Count;
	public T Item;
}

/// A pair of array fields for each element type and rank a case reaches. Each
/// pair holds one array, so the two receivers name one object.
class Arrays {
	public Vec4[] First = new Vec4 [4];
	public Vec4[] Second = new Vec4 [4];
	public Vec4[,] FirstGrid = new Vec4 [2, 2];
	public Vec4[,] SecondGrid = new Vec4 [2, 2];
	public double[] FirstFlat = new double [4];
	public double[] SecondFlat = new double [4];
}

static class Program {
	static int fails;

	static void Check (string what, long got, long want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL: {0} is {1}, wanted {2}", what, got, want);
		++fails;
	}

	static void Check (string what, double got, double want)
	{
		Check (what, BitConverter.DoubleToInt64Bits (got),
			BitConverter.DoubleToInt64Bits (want));
	}

	/*
	 * Volatile.Write is `Unsafe.As<int, VolatileInt32> (ref location).Value =
	 * value`, so the store is stfld on a byref whose type the caller asserted.
	 * The load below is ldfld on the real field. A tag that trusts the byref
	 * puts the two on different leaves and the store is lost.
	 *
	 * a and b are one object. The addresses are two arguments, so nothing but
	 * the tag connects them.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int VolatileTwoRefs (Counter a, Counter b, int value)
	{
		int before = b.Value;

		Volatile.Write (ref a.Value, value);

		return before + b.Value;
	}

	/*
	 * One array in two fields. Both accesses are the same field of the same
	 * value type, so they are one leaf however the arrays are reached.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static double SameFieldTwoArrays (Vec4[] first, Vec4[] second, double value)
	{
		double before = second [1].A;

		first [1].A = value;

		return before + second [1].A;
	}

	/// The same shape on a rank-2 array, which reaches its elements through the
	/// Address accessor rather than ldelema.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static double SameFieldTwoGrids (Vec4[,] first, Vec4[,] second, double value)
	{
		double before = second [1, 1].B;

		first [1, 1].B = value;

		return before + second [1, 1].B;
	}

	/*
	 * Two fields of one value type do not overlap, so the four stores and the
	 * four loads below are eight leaves. The values still have to come back,
	 * which is what a wrong offset or a wrong key breaks.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static double DistinctFields (Vec4[] first, Vec4[] second)
	{
		first [0].A = 1.0;
		first [0].B = 2.0;
		first [0].C = 4.0;
		first [0].D = 8.0;

		return second [0].A + second [0].B + second [0].C + second [0].D;
	}

	/// One object in two references, on a class field rather than a value type.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SameFieldTwoObjects (Counter a, Counter b, int value)
	{
		int before = b.Value;

		a.Value = value;

		return before + b.Value;
	}

	/*
	 * A field the base class declares, reached through a base reference and a
	 * derived one. Both tokens name Counter::Value, so both resolve to one
	 * field and land on one leaf.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int InheritedField (Derived d, Counter c, int value)
	{
		int before = c.Value;

		d.Value = value;

		return before + c.Value;
	}

	/// One array in two fields, read and written as whole elements. This is the
	/// element leaf rather than the field leaf.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static double SameElementTwoArrays (double[] first, double[] second, double value)
	{
		double before = second [2];

		first [2] = value;

		return before + second [2];
	}

	/*
	 * A whole element of a value-type array is a struct copy, which carries no
	 * tag. It has to keep aliasing the field leaves, because the bytes it moves
	 * are those fields.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static double WholeElementSeesField (Vec4[] array, double value)
	{
		array [3].C = value;

		Vec4 copy = array [3];

		return copy.C;
	}

	/*
	 * The same field of one generic type, reached through a shared body and a
	 * specialised one. Box<string> shares its body and Box<int> does not.
	 *
	 * A shared body names a field's class in its open form, not the
	 * instantiation it runs as, so it cannot say which storage the field reaches
	 * and takes the coarse leaf. depends_on_context () decides that
	 * (method-to-llvm/generic-sharing.cpp).
	 *
	 * This case covers both spellings. It cannot fail on its own, because the
	 * two instantiations are different objects and never alias. Read BumpBox's
	 * IR at whatever tier it reaches to see which leaf each one took.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int BumpBox<T> (Box<T> a, Box<T> b, int value)
	{
		int before = b.Count;

		a.Count = value;

		return before + b.Count;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SharedGenericField (int value)
	{
		Box<string> shared = new Box<string> ();
		Box<int> exact = new Box<int> ();

		return BumpBox (shared, shared, value) + BumpBox (exact, exact, value);
	}

	static void RunAll (string tier)
	{
		Counter counter = new Counter ();
		Derived derived = new Derived ();
		Arrays arrays = new Arrays ();

		// One array in both fields of each pair, which is the shape a numeric
		// kernel reaches when two of its arrays turn out to be the same one.
		arrays.Second = arrays.First;
		arrays.SecondGrid = arrays.FirstGrid;
		arrays.SecondFlat = arrays.FirstFlat;

		counter.Value = 1;
		Check (tier + " volatile two refs", VolatileTwoRefs (counter, counter, 7), 8);

		arrays.First [1].A = 1.0;
		Check (tier + " same field two arrays",
			SameFieldTwoArrays (arrays.First, arrays.Second, 7.0), 8.0);

		arrays.FirstGrid [1, 1].B = 1.0;
		Check (tier + " same field two grids",
			SameFieldTwoGrids (arrays.FirstGrid, arrays.SecondGrid, 7.0), 8.0);

		Check (tier + " distinct fields",
			DistinctFields (arrays.First, arrays.Second), 15.0);

		counter.Value = 1;
		Check (tier + " same field two objects",
			SameFieldTwoObjects (counter, counter, 7), 8);

		derived.Value = 1;
		Check (tier + " inherited field", InheritedField (derived, derived, 7), 8);

		arrays.FirstFlat [2] = 1.0;
		Check (tier + " same element two arrays",
			SameElementTwoArrays (arrays.FirstFlat, arrays.SecondFlat, 7.0), 8.0);

		Check (tier + " whole element sees field",
			WholeElementSeesField (arrays.First, 9.0), 9.0);

		Check (tier + " shared generic field", SharedGenericField (7), 14);
	}

	static bool Promote (string name, int tier)
	{
		MethodInfo method = typeof (Program).GetMethod (name,
			BindingFlags.Static | BindingFlags.NonPublic);

		if (Mono.Tiering.MonoTier.PromoteNow (method.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: {0} would not compile at tier {1}", name, tier);
		++fails;
		return false;
	}

	static readonly string[] cases = {
		"VolatileTwoRefs", "SameFieldTwoArrays", "SameFieldTwoGrids",
		"DistinctFields", "SameFieldTwoObjects", "InheritedField",
		"SameElementTwoArrays", "WholeElementSeesField", "SharedGenericField",
	};

	public static int Main ()
	{
		RunAll ("tier0");

		// MonoTier::tier1 is 2 and tier2 is 3 (mono/mini/domain-method.hpp).
		foreach (string name in cases)
			Promote (name, 2);
		RunAll ("tier1");

		foreach (string name in cases)
			Promote (name, 3);
		RunAll ("tier2");

		Console.WriteLine (fails == 0 ? "OK" : fails + " failures");
		return fails == 0 ? 0 : 1;
	}
}
