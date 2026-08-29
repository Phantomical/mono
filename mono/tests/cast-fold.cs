using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

/*
 * A type test the compiler answers from what the IR says about the operand.
 *
 * fold_type_tests () reads the class an argument's slot is declared with, or the
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

class Field { public object Held; }

sealed class Sealed : Base { }

class Holder<T> { }
class IntHolder : Holder<int>, IMarker { }

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

	// A closed generic instance bounds its slot the way an ordinary class does.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool ListIsCollection (List<int> l) => l is ICollection<int>;

	// Two instantiations of one generic share no class, so this is answerable no.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HolderIntIsHolderString (Holder<int> h) => h is Holder<string>;

	// A bound says nothing about a class under it.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HolderIntIsIntHolder (Holder<int> h) => h is IntHolder;

	// A subclass may implement any interface, whether or not the bound is a
	// generic instance.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HolderIntIsMarker (Holder<int> h) => h is IMarker;

	/*
	 * One shared body serves both calls below, so its `Holder<T>` parameter
	 * states no class at all. A body that answered off the shared signature
	 * would answer the two the same, and the two disagree.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsHolderOfObject<T> (Holder<T> h) => h is Holder<object>;

	/*
	 * A closed instance beside the shared parameter is the class every
	 * instantiation gets. A shared body states it, so both arms of the rule
	 * are answerable off it.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool SharedListIsCollection<T> (List<int> l, T ignored) => l is ICollection<int>;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool SharedListIsListOfString<T> (List<int> l, T ignored) => l is List<string>;

	// The class of a fresh object is exact, so both directions are answerable.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FreshDerivedIsBase () => new Derived () is Base;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FreshBaseIsDerived () => new Base () is Derived;

	/*
	 * A fresh array is exact as well, which is the one array operand the bound
	 * arms above do not cover. Each test goes through `object` so that C#
	 * answers none of them itself.
	 *
	 * `uint[]` is the arm that tests assignability itself. The two classes
	 * differ but share a cast class, so comparing them directly answers no,
	 * while the runtime answers yes.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FreshIntArrayIsIList () => (object) new int[2] is IList<int>;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FreshIntArrayIsUIntArray () => (object) new int[2] is uint[];

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FreshIntArrayIsComparable () => (object) new int[2] is IComparable;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FreshIntArrayIsUnrelated () => (object) new int[2] is Unrelated;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FreshDerivedArrayIsMarkerArray () => (object) new Derived[1] is IMarker[];

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FreshBaseArrayIsMarkerArray () => (object) new Base[1] is IMarker[];

	/*
	 * A call between a store and a load of what it stored leaves `sources ()`
	 * naming the stored object and reporting that it did not name every value,
	 * because the call is free to have written the field as well. Answering the
	 * test off the named value alone gives `true` here, and the call is what
	 * makes `false` right.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Overwrite (Field f) => f.Held = new Unrelated ();

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool OverwrittenFieldIsBase (Field f)
	{
		f.Held = new Derived ();
		Overwrite (f);
		return f.Held is Base;
	}

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

		Holder<int> held = new Holder<int> ();
		Holder<int> held_derived = new IntHolder ();

		Check ("List<int> is ICollection<int>", ListIsCollection (new List<int> ()), true);
		Check ("null List<int> is ICollection<int>", ListIsCollection (null), false);

		Check ("Holder<int> is Holder<string>", HolderIntIsHolderString (held), false);
		Check ("Holder<int>(IntHolder) is Holder<string>",
		       HolderIntIsHolderString (held_derived), false);

		Check ("Holder<int>(IntHolder) is IntHolder", HolderIntIsIntHolder (held_derived), true);
		Check ("Holder<int>(Holder<int>) is IntHolder", HolderIntIsIntHolder (held), false);

		Check ("Holder<int>(IntHolder) is IMarker", HolderIntIsMarker (held_derived), true);
		Check ("Holder<int>(Holder<int>) is IMarker", HolderIntIsMarker (held), false);

		Check ("shared Holder<object> is Holder<object>",
		       IsHolderOfObject<object> (new Holder<object> ()), true);
		Check ("shared Holder<string> is Holder<object>",
		       IsHolderOfObject<string> (new Holder<string> ()), false);

		Check ("shared List<int> is ICollection<int>",
		       SharedListIsCollection<string> (new List<int> (), null), true);
		Check ("shared List<int> is List<string>",
		       SharedListIsListOfString<string> (new List<int> (), null), false);

		Check ("fresh Derived is Base", FreshDerivedIsBase (), true);
		Check ("fresh Base is Derived", FreshBaseIsDerived (), false);

		Check ("fresh int[] is IList<int>", FreshIntArrayIsIList (), true);
		Check ("fresh int[] is uint[]", FreshIntArrayIsUIntArray (), true);
		Check ("fresh int[] is IComparable", FreshIntArrayIsComparable (), false);
		Check ("fresh int[] is Unrelated", FreshIntArrayIsUnrelated (), false);
		Check ("fresh Derived[] is IMarker[]", FreshDerivedArrayIsMarkerArray (), true);
		Check ("fresh Base[] is IMarker[]", FreshBaseArrayIsMarkerArray (), false);

		Check ("overwritten field is Base", OverwrittenFieldIsBase (new Field ()), false);
	}

	public static int Main ()
	{
		// The first rounds run interpreted, and the later ones run whatever the
		// thresholds promoted. Both answer through this same code.
		//
		// The count reaches tier 2, which is where the fold answers these
		// tests. A tier-1 body keeps the icall at each of them, so a count that
		// stops at tier 1 gates the runtime alone.
		for (int i = 0; i < 25000; ++i)
			Round ();

		if (failures != 0) {
			Console.WriteLine ("{0} wrong answers", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
