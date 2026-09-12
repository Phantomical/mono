using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

// Several cases below assert an `is` result the C# compiler can already
// tell is always false, which triggers CS0184.
#pragma warning disable CS0184

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

public class CastEliminate {
	static int failures;

	static void Check (string what, bool got, bool want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
		++failures;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsIntArray (int[] a) => a is int[];

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsIList (int[] a) => a is IList<int>;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsIComparable (int[] a) => a is IComparable;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsUnrelatedClass (int[] a) => a is Unrelated;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsMarkerArray (Base[] a) => a is IMarker[];

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool BaseIsUnrelated (Base b) => b is Unrelated;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool BaseIsMarker (Base b) => b is IMarker;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool DerivedIsBase (Derived d) => d is Base;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool BaseIsDerived (Base b) => b is Derived;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object CastToBase (Base b) => (Base) b;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object CastToDerived (Base b) => (Derived) b;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool ListIsCollection (List<int> l) => l is ICollection<int>;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HolderIntIsHolderString (Holder<int> h) => h is Holder<string>;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HolderIntIsIntHolder (Holder<int> h) => h is IntHolder;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HolderIntIsMarker (Holder<int> h) => h is IMarker;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool IsHolderOfObject<T> (Holder<T> h) => h is Holder<object>;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool SharedListIsCollection<T> (List<int> l, T ignored) => l is ICollection<int>;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool SharedListIsListOfString<T> (List<int> l, T ignored) => l is List<string>;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FreshDerivedIsBase () => new Derived () is Base;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool FreshBaseIsDerived () => new Base () is Derived;

	// Cast through `object` so the C# compiler cannot constant-fold these
	// itself; the point is to test this backend's elimination, not Roslyn's.
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
		// Long enough to reach tier 2's promotion threshold in every
		// configuration that promotes: some cases above are still unresolved
		// at tier 1.
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
