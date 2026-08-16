// Generic methods and generic types: what carries a type argument into an
// opcode - constrained calls, ldtoken, newarr, box - and the tables a generic
// virtual call resolves through. A shape that can differ between a reference
// and a value type argument is written over both.
//
// Operands go through NoInlining helpers, so the transform cannot fold a test
// into its answer.

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

public class Generics {

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Id (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long IdL (long x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static double IdD (double x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string IdS (string s) { return s; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object IdO (object o) { return o; }

	struct GenPoint : IComparable<GenPoint> {
		public int X, Y;
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int CompareTo (GenPoint other) { return X - other.X; }
	}

	struct GenPlain { public int V; }

	struct GenOver {
		public int V;
		public override string ToString () { return "over"; }
	}

	class GenNamed {
		public override string ToString () { return "named"; }
	}

	interface GenICount { int Count (); }

	struct GenCountVal : GenICount {
		public int N;
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Count () { return N; }
	}

	class GenCountRef : GenICount {
		public int N;
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Count () { return N; }
	}

	interface GenIBox<T> { T Get (); }

	class GenBoxRef<T> : GenIBox<T> {
		public T V;
		[MethodImpl (MethodImplOptions.NoInlining)]
		public T Get () { return V; }
	}

	struct GenBoxVal<T> : GenIBox<T> {
		public T V;
		[MethodImpl (MethodImplOptions.NoInlining)]
		public T Get () { return V; }
	}

	class GenHolder<T> {
		public T V;

		[MethodImpl (MethodImplOptions.NoInlining)]
		public T Get () { return V; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Widths<U> ()
		{
			return (typeof (T).IsValueType ? 1 : 0) + (typeof (U).IsValueType ? 2 : 0);
		}
	}

	struct GenCell<T> : GenICount {
		public T V;
		public int Tag;

		[MethodImpl (MethodImplOptions.NoInlining)]
		public T Get () { return V; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Count () { return Tag; }
	}

	class GenPickBase {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public virtual T Pick<T> (T a, T b) { return a; }
	}

	class GenPickDerived : GenPickBase {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public override T Pick<T> (T a, T b) { return b; }
	}

	interface GenIPick { T Pick<T> (T a, T b); }

	class GenPicker : GenIPick {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public T Pick<T> (T a, T b) { return b; }
	}

	class GenRank<T> {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public virtual int Rank<U> (T t, U u) { return 1; }
	}

	class GenRankInt : GenRank<int> {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public override int Rank<U> (int t, U u) { return t + (typeof (U).IsValueType ? 2 : 0); }
	}

	class GenMade {
		public int N;
		public GenMade () { N = 5; }
	}

	static int gen_cctor_order;

	class GenCctor<T> { public static readonly int Value = ++gen_cctor_order; }

	[Flags] enum GenFlags { None = 0, A = 1, B = 2, C = 4 }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static T GenIdT<T> (T v) { return v; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static U GenSecond<T, U> (T a, U b) { return b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenLen<T> (T[] a) { return a.Length; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static T GenDefault<T> () { return default (T); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenCallCount<T> (T t) where T : GenICount { return t.Count (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenCompare<T> (T a, T b) where T : IComparable<T> { return a.CompareTo (b); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string GenToString<T> (T t) { return t.ToString (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenHash<T> (T t) { return t.GetHashCode (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Type GenGetType<T> (T t) { return t.GetType (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Type GenTypeOf<T> () { return typeof (T); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenIsValueType<T> () { return typeof (T).IsValueType ? 1 : 0; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static T[] GenNewArray<T> (int n) { return new T[n]; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void GenStoreFirst<T> (T[] a, T v) { a[0] = v; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object GenBoxT<T> (T v) { return v; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static T GenUnboxT<T> (object o) { return (T) o; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void GenSwap<T> (ref T a, ref T b) { T t = a; a = b; b = t; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static T GenCreate<T> () where T : new() { return new T (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenEqual<T> (T a, T b)
	{
		return EqualityComparer<T>.Default.Equals (a, b) ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenPickVia<T> (GenPickBase b, T x, T y) where T : class
	{
		return b.Pick<T> (x, y) == x ? 1 : 2;
	}

	public static int test_7_generic_method_over_i4 ()
	{
		return GenIdT<int> (Id (7));
	}

	public static int test_3_generic_method_over_a_reference_type ()
	{
		return GenIdT<string> (IdS ("abc")).Length;
	}

	public static int test_11_generic_method_over_i8_and_r8 ()
	{
		// The high word and the fraction both have to survive, so a value that a
		// narrowed slot would truncate gives the wrong answer.
		long l = GenIdT<long> (IdL (0x100000005L));
		double d = GenIdT<double> (IdD (2.5));

		return (int) (l >> 32) + (int) (d * IdD (4));
	}

	public static int test_11_generic_method_returns_a_struct ()
	{
		GenPoint p = GenIdT<GenPoint> (new GenPoint { X = Id (4), Y = Id (7) });
		return p.X + p.Y;
	}

	public static int test_9_generic_method_with_two_type_arguments ()
	{
		return GenSecond<string, int> (IdS ("x"), Id (9));
	}

	public static int test_7_generic_method_over_an_array_type ()
	{
		return GenLen<int> (new int[Id (4)]) + GenIdT<int[]> (new int[Id (3)]).Length;
	}

	public static int test_9_generic_class_over_a_value_type ()
	{
		return new GenHolder<int> { V = Id (9) }.Get ();
	}

	public static int test_4_generic_class_over_a_reference_type ()
	{
		return new GenHolder<string> { V = IdS ("abcd") }.Get ().Length;
	}

	public static int test_2_generic_class_with_a_generic_method ()
	{
		// The class type argument is a reference type and the method one is not,
		// so a mix-up of the two answers 1 instead of 2.
		return new GenHolder<string> ().Widths<GenPoint> ();
	}

	public static int test_0_default_of_the_type_argument ()
	{
		if (GenDefault<string> () != null)
			return 1;

		GenPoint p = GenDefault<GenPoint> ();
		return GenDefault<int> () + p.X + p.Y;
	}

	public static int test_1_default_of_a_nullable_has_no_value ()
	{
		return GenDefault<int?> ().HasValue ? 0 : 1;
	}

	public static int test_3_constrained_call_on_a_struct ()
	{
		return GenCallCount<GenCountVal> (new GenCountVal { N = Id (3) });
	}

	public static int test_5_constrained_call_on_a_class ()
	{
		return GenCallCount<GenCountRef> (new GenCountRef { N = Id (5) });
	}

	public static int test_6_constrained_call_on_a_generic_struct ()
	{
		return GenCallCount<GenCell<string>> (new GenCell<string> { Tag = Id (6) });
	}

	public static int test_5_constrained_call_through_a_generic_interface ()
	{
		return GenCompare<GenPoint> (new GenPoint { X = Id (7) }, new GenPoint { X = Id (2) });
	}

	public static int test_1_constrained_tostring_boxes_a_struct ()
	{
		// GenPlain does not override ToString, so the call lands on ValueType and
		// boxes the receiver. The answer is the type name.
		string s = GenToString<GenPlain> (new GenPlain { V = Id (1) });
		return s == typeof (GenPlain).FullName ? 1 : 0;
	}

	public static int test_1_constrained_tostring_on_an_overriding_struct ()
	{
		return GenToString<GenOver> (new GenOver { V = Id (1) }) == "over" ? 1 : 0;
	}

	public static int test_1_constrained_tostring_on_a_class ()
	{
		return GenToString<GenNamed> (new GenNamed ()) == "named" ? 1 : 0;
	}

	public static int test_4_constrained_gethashcode_on_an_enum ()
	{
		// The underlying int is its own hash. The constrained call resolves to
		// Int32 rather than to Enum, so nothing is boxed.
		return GenHash<GenFlags> ((GenFlags) Id (4));
	}

	public static int test_1_constrained_gettype_on_a_nullable_with_a_value ()
	{
		// A boxed nullable is a boxed int, so the answer names the underlying type.
		return GenGetType<int?> (Id (5)) == typeof (int) ? 1 : 0;
	}

	public static int test_1_constrained_gettype_on_an_empty_nullable_throws ()
	{
		try {
			GenGetType<int?> (GenDefault<int?> ());
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_7_generic_interface_on_a_class ()
	{
		GenIBox<int> b = new GenBoxRef<int> { V = Id (7) };
		return b.Get ();
	}

	public static int test_5_generic_interface_on_a_boxed_struct ()
	{
		GenIBox<int> b = new GenBoxVal<int> { V = Id (5) };
		return b.Get ();
	}

	public static int test_1_isinst_on_a_generic_interface ()
	{
		object o = IdO (new GenBoxRef<int> ());
		return o is GenIBox<int> && !(o is GenIBox<string>) ? 1 : 0;
	}

	public static int test_6_generic_virtual_method_is_overridden ()
	{
		GenPickBase b = new GenPickDerived ();
		return b.Pick<int> (Id (1), Id (6));
	}

	public static int test_2_generic_virtual_method_on_the_base ()
	{
		GenPickBase b = new GenPickBase ();
		return b.Pick<int> (Id (2), Id (9));
	}

	public static int test_3_generic_virtual_two_instantiations_share_a_slot ()
	{
		// One receiver, one vtable slot, two instantiations, so the slot holds a
		// list rather than a single method.
		GenPickBase b = new GenPickDerived ();
		return b.Pick<int> (Id (9), Id (1)) + b.Pick<string> (IdS ("a"), IdS ("xy")).Length;
	}

	public static int test_11_generic_virtual_over_a_struct ()
	{
		GenPickBase b = new GenPickDerived ();
		GenPoint p = b.Pick<GenPoint> (new GenPoint { X = Id (1) },
		                               new GenPoint { X = Id (4), Y = Id (7) });
		return p.X + p.Y;
	}

	public static int test_2_generic_virtual_from_a_generic_caller ()
	{
		GenPickBase b = new GenPickDerived ();
		return GenPickVia<string> (b, IdS ("a"), IdS ("b"));
	}

	public static int test_5_generic_virtual_on_a_generic_class ()
	{
		GenRank<int> h = new GenRankInt ();
		return h.Rank<GenPoint> (Id (3), new GenPoint ());
	}

	public static int test_5_generic_method_on_an_interface ()
	{
		GenIPick g = new GenPicker ();
		return g.Pick<int> (Id (1), Id (5));
	}

	public static int test_6_nested_generic_type ()
	{
		List<List<int>> outer = new List<List<int>> ();

		outer.Add (new List<int> { Id (1), Id (2) });
		outer.Add (new List<int> { Id (3) });
		return outer[0][0] + outer[0][1] + outer[1][0];
	}

	public static int test_8_generic_struct_over_a_value_type ()
	{
		GenCell<int> c = new GenCell<int> { V = Id (5), Tag = Id (3) };
		return c.Get () + c.Tag;
	}

	public static int test_11_generic_struct_over_a_struct ()
	{
		GenCell<GenPoint> c = new GenCell<GenPoint> {
			V = new GenPoint { X = Id (4), Y = Id (7) }
		};
		return c.Get ().X + c.Get ().Y;
	}

	public static int test_1_typeof_the_type_argument ()
	{
		if (GenTypeOf<int> () != typeof (int))
			return 0;
		return GenIsValueType<int> () - GenIsValueType<string> ();
	}

	public static int test_2_new_array_of_the_type_argument_starts_empty ()
	{
		string[] s = GenNewArray<string> (Id (2));
		GenPoint[] p = GenNewArray<GenPoint> (Id (2));

		if (s[1] != null)
			return 0;
		return s.Length + p.Length - p[1].X - p[1].Y - 2;
	}

	public static int test_9_array_of_the_type_argument_round_trips ()
	{
		GenPoint[] a = GenNewArray<GenPoint> (Id (2));

		GenStoreFirst<GenPoint> (a, new GenPoint { X = Id (2), Y = Id (7) });
		return a[0].X + a[0].Y;
	}

	public static int test_1_array_of_the_type_argument_rejects_a_wider_element ()
	{
		try {
			GenStoreFirst<object> (new string[Id (2)], IdO (new GenNamed ()));
			return 0;
		} catch (ArrayTypeMismatchException) {
			return 1;
		}
	}

	public static int test_6_box_and_unbox_the_type_argument ()
	{
		return GenUnboxT<int> (GenBoxT<int> (Id (6)));
	}

	public static int test_1_unbox_of_a_wrong_type_argument_throws ()
	{
		try {
			return GenUnboxT<int> (IdO (IdS ("x")));
		} catch (InvalidCastException) {
			return 1;
		}
	}

	public static int test_2_variant_interface_dispatch ()
	{
		IEnumerable<object> e = (IEnumerable<object>) IdO (new List<string> { "a", "b" });
		int n = 0;

		foreach (object item in e)
			n++;
		return n;
	}

	public static int test_3_cctor_runs_per_instantiation ()
	{
		// The two reference instantiations are the interesting pair: they must
		// still get a static of their own. The test compares the values rather
		// than summing them, because beforefieldinit leaves the order of the
		// three free.
		int a = GenCctor<int>.Value, b = GenCctor<string>.Value, c = GenCctor<object>.Value;

		return a != b && b != c && a != c ? gen_cctor_order : 0;
	}

	public static int test_7_generic_delegate_over_an_instantiation ()
	{
		Func<int, int> f = GenIdT<int>;
		return f (Id (7));
	}

	public static int test_1_default_comparer_over_a_value_type ()
	{
		// A comparer that answers true for everything must not pass, so the test
		// subtracts the unequal pair rather than leaving it out.
		return GenEqual<int> (Id (4), Id (4)) - GenEqual<int> (Id (4), Id (5));
	}

	public static int test_5_new_constraint_creates_the_type_argument ()
	{
		// GenMade sets N in its constructor, so a zeroed instance answers 0.
		GenPoint p = GenCreate<GenPoint> ();

		return GenCreate<GenMade> ().N + p.X + p.Y;
	}

	public static int test_28_by_ref_type_argument ()
	{
		int a = Id (1), b = Id (2);
		GenPoint p = new GenPoint { X = Id (3) }, q = new GenPoint { Y = Id (4) };

		GenSwap<int> (ref a, ref b);
		GenSwap<GenPoint> (ref p, ref q);
		return a * 10 + b + p.Y + q.X;
	}
}
