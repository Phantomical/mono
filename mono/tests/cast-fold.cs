using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

/*
 * A type test the compiler answers from what the IR says about the operand.
 *
 * FoldCastPass reads the class an argument's slot is declared with, or the
 * class an allocation made, and answers the test for every class that slot
 * admits. Each case below is one arm of that rule, and each is written so that
 * a wrong answer is a wrong value rather than a slower one.
 *
 * Every case runs interpreted and compiled in one process. The interpreter
 * answers each test through the runtime, so a fold that disagrees with it fails
 * here whatever tier it happened at.
 */

enum E32 : int { A, B }
enum E8 : byte { A, B }

interface IMarker { }

class Base { }
class Derived : Base, IMarker { }
class Unrelated { }

sealed class Sealed : Base { }

public class CastFold {
	static int failures;

	static void Check (string what, bool got, bool want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
		++failures;
	}

	/*
	 * An array slot admits every array whose rank and cast class match, so
	 * `int[]`, `uint[]` and an enum array over int all reach here. The rule
	 * answers each test off `int[]` alone, and all three have to agree with it.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsIntArray (int[] a) => a is int[];

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsIList (int[] a) => a is IList<int>;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsIComparable (int[] a) => a is IComparable;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsUnrelatedClass (int[] a) => a is Unrelated;

	// The element type is not sealed, so this slot admits arrays whose element
	// classes differ. A test the element decides must not be answered off
	// `Base[]`.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsMarkerArray (Base[] a) => a is IMarker[];

	// Single inheritance: no class is under both, so this is answerable no.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool BaseIsUnrelated (Base b) => b is Unrelated;

	// A subclass may implement any interface, so a bound answers nothing here.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool BaseIsMarker (Base b) => b is IMarker;

	// Assignability carries down, so every class the slot admits passes.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool DerivedIsBase (Derived d) => d is Base;

	// A bound says nothing about a class under it.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool BaseIsDerived (Base b) => b is Derived;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object CastToBase (Base b) => (Base) b;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object CastToDerived (Base b) => (Derived) b;

	// The class of a fresh object is exact, so both directions are answerable.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FreshDerivedIsBase () => new Derived () is Base;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FreshBaseIsDerived () => new Base () is Derived;

	static void Round ()
	{
		int[] ints = new int[2];
		uint[] uints = new uint[2];
		E32[] enums = new E32[2];
		E8[] bytes = new E8[2];

		// All three reach an `int[]` slot, and the runtime says yes to each.
		Check ("int[] is int[]", IsIntArray (ints), true);
		Check ("uint[] is int[]", IsIntArray ((int[]) (object) uints), true);
		Check ("E32[] is int[]", IsIntArray ((int[]) (object) enums), true);

		Check ("int[] is IList<int>", IsIList (ints), true);
		Check ("uint[] is IList<int>", IsIList ((int[]) (object) uints), true);
		Check ("E32[] is IList<int>", IsIList ((int[]) (object) enums), true);

		Check ("int[] is IComparable", IsIComparable (ints), false);
		Check ("uint[] is IComparable", IsIComparable ((int[]) (object) uints), false);

		Check ("int[] is Unrelated", IsUnrelatedClass (ints), false);
		Check ("null is int[]", IsIntArray (null), false);
		Check ("null is IList<int>", IsIList (null), false);

		// E8[] does not reach an int[] slot at all, which is what keeps the
		// cast class the whole story for the ones that do.
		Check ("E8[] is int[] the long way", bytes is int[], false);

		Check ("Derived[] is IMarker[]", IsMarkerArray (new Derived[1]), true);
		Check ("Base[] is IMarker[]", IsMarkerArray (new Base[1]), false);

		Base derived = new Derived ();
		Base plain = new Base ();
		Base sealed_ = new Sealed ();

		Check ("Base(Derived) is Unrelated", BaseIsUnrelated (derived), false);
		Check ("Base(Base) is Unrelated", BaseIsUnrelated (plain), false);
		Check ("null is Unrelated", BaseIsUnrelated (null), false);

		Check ("Base(Derived) is IMarker", BaseIsMarker (derived), true);
		Check ("Base(Base) is IMarker", BaseIsMarker (plain), false);
		Check ("Base(Sealed) is IMarker", BaseIsMarker (sealed_), false);

		Check ("Derived is Base", DerivedIsBase ((Derived) derived), true);
		Check ("null Derived is Base", DerivedIsBase (null), false);

		Check ("Base(Derived) is Derived", BaseIsDerived (derived), true);
		Check ("Base(Base) is Derived", BaseIsDerived (plain), false);

		Check ("cast Base(Derived) to Base", CastToBase (derived) != null, true);
		Check ("cast null to Base", CastToBase (null) == null, true);
		Check ("cast Base(Derived) to Derived", CastToDerived (derived) != null, true);
		Check ("cast null to Derived", CastToDerived (null) == null, true);

		bool threw = false;

		try {
			CastToDerived (plain);
		} catch (InvalidCastException) {
			threw = true;
		}

		Check ("cast Base(Base) to Derived throws", threw, true);

		Check ("fresh Derived is Base", FreshDerivedIsBase (), true);
		Check ("fresh Base is Derived", FreshBaseIsDerived (), false);
	}

	public static int Main ()
	{
		// The first rounds run interpreted, and the later ones run whatever the
		// thresholds promoted. Both answer through this same code.
		for (int i = 0; i < 200; ++i)
			Round ();

		if (failures != 0) {
			Console.WriteLine ("{0} wrong answers", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
