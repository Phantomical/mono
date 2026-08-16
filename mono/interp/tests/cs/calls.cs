// The call family: static, instance, virtual, interface and constrained calls,
// the width of each argument and each return value, and the transform's
// decisions about the receiver -- null check, unbox or box -- and about
// inlining.
//
// A method named test_<n>_<what> is a test. It passes when it returns <n>.
// Outside the inlining group every callee is NoInlining, so the transform
// cannot fold a site into its answer.
//
// Where a test makes more than one call, each call carries a different weight
// into the answer. A sum gives the same total when two slots or two fields
// exchange places, and a test of layout that cannot see that tests nothing.

using System;
using System.Runtime.CompilerServices;

public class Calls {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long IdL (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double IdD (double x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static object IdO (object x) { return x; }

	// ---------------------------------------------------------------- dispatch

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int AddStatic (int a, int b) { return a + b; }

	public static int test_7_static_call ()
	{
		return AddStatic (Id (3), Id (4));
	}

	class CallsPlainClass {
		public int bias;
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Tag (int n) { return bias + n; }
	}

	public static int test_5_instance_call_on_a_class ()
	{
		CallsPlainClass o = new CallsPlainClass ();
		o.bias = Id (2);
		return o.Tag (Id (3));
	}

	class CallsBase {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public virtual int Val () { return 9; }
	}

	class CallsDerived : CallsBase {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public override int Val () { return 2; }
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int FromBase () { return base.Val (); }
	}

	public static int test_29_callvirt_reads_the_receivers_slot ()
	{
		CallsBase over = new CallsDerived (), plain = new CallsBase ();
		return over.Val () * 10 + plain.Val ();
	}

	// base.Val () is a plain call on a virtual method, so it must not dispatch.
	public static int test_9_base_call_skips_the_override ()
	{
		return new CallsDerived ().FromBase ();
	}

	class CallsSealedBase {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public virtual int Rank () { return 1; }
	}

	class CallsSealedLeaf : CallsSealedBase {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public sealed override int Rank () { return 4; }
	}

	public static int test_4_sealed_override ()
	{
		CallsSealedLeaf leaf = new CallsSealedLeaf ();
		return leaf.Rank ();
	}

	abstract class CallsShape {
		public abstract int Sides ();
	}

	class CallsHex : CallsShape {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public override int Sides () { return 6; }
	}

	public static int test_6_abstract_callvirt ()
	{
		CallsShape s = new CallsHex ();
		return s.Sides ();
	}

	interface CallsIWeight { int Weight (); }

	class CallsHeavy : CallsIWeight {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Weight () { return 10; }
	}

	class CallsLight : CallsIWeight {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Weight () { return 2; }
	}

	class CallsExplicit : CallsIWeight {
		[MethodImpl (MethodImplOptions.NoInlining)]
		int CallsIWeight.Weight () { return 3; }
	}

	// One site with two receiver classes, so no single target fits it.
	public static int test_12_two_receivers_at_one_site ()
	{
		CallsIWeight[] all = { new CallsHeavy (), new CallsLight () };
		int total = 0;

		foreach (CallsIWeight w in all)
			total += w.Weight ();
		return total;
	}

	interface CallsIA { int A (); }
	interface CallsIB { int B (); }

	class CallsAB : CallsIA, CallsIB {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int A () { return 1; }
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int B () { return 2; }
	}

	// Two interfaces on one receiver, and an explicit implementation that
	// nothing but the interface can reach.
	public static int test_123_interface_calls_reach_their_slots ()
	{
		object o = IdO (new CallsAB ());
		CallsIWeight w = new CallsExplicit ();

		return ((CallsIA) o).A () * 100 + ((CallsIB) o).B () * 10 + w.Weight ();
	}

	interface CallsIMaker<out T> { T Make (); }

	class CallsStringMaker : CallsIMaker<string> {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public string Make () { return "ok"; }
	}

	// The receiver implements CallsIMaker<string>, so the interface offset is
	// found by variance rather than by an exact match.
	public static int test_2_variant_interface_call ()
	{
		CallsIMaker<object> m = new CallsStringMaker ();
		return ((string) m.Make ()).Length;
	}

	class CallsGvBase {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public virtual int Pick<T> (T v) { return 1; }
	}

	class CallsGvDerived : CallsGvBase {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public override int Pick<T> (T v) { return 2; }
	}

	// Two instantiations share one vtable slot, so the slot holds a list.
	public static int test_22_generic_virtual_method ()
	{
		CallsGvBase b = new CallsGvDerived ();
		return b.Pick<int> (Id (0)) * 10 + b.Pick<object> (null);
	}

	// ------------------------------------------------------------ null checks

	[MethodImpl (MethodImplOptions.NoInlining)] static CallsBase NullBase () { return null; }
	[MethodImpl (MethodImplOptions.NoInlining)] static CallsPlainClass NullPlain () { return null; }
	[MethodImpl (MethodImplOptions.NoInlining)] static CallsIWeight NullWeight () { return null; }

	public static int test_1_callvirt_on_a_null_receiver ()
	{
		try {
			NullBase ().Val ();
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_1_non_virtual_callvirt_on_a_null_receiver ()
	{
		try {
			NullPlain ().Tag (1);
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_1_interface_call_on_a_null_receiver ()
	{
		try {
			NullWeight ().Weight ();
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_1_constrained_call_on_a_null_receiver ()
	{
		try {
			WeightOf<CallsHeavy> (null);
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	// -------------------------------------------------------------- recursion

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Depth (int n) { return n == 0 ? 0 : 1 + Depth (n - 1); }

	public static int test_300_deep_recursion ()
	{
		return Depth (Id (300));
	}

	abstract class CallsCount { public abstract int Down (int n); }

	class CallsCountImpl : CallsCount {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public override int Down (int n) { return n == 0 ? 0 : 1 + Down (n - 1); }
	}

	public static int test_20_virtual_recursion ()
	{
		CallsCount c = new CallsCountImpl ();
		return c.Down (Id (20));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long DeepWide (int n, CallsS40 s)
	{
		return n == 0 ? s.a : DeepWide (n - 1, s);
	}

	public static int test_7_recursion_with_a_wide_frame ()
	{
		return (int) DeepWide (Id (100), MakeS40 (Id (7), 11, 12, 13, 14));
	}

	// -------------------------------------------------------------- arguments

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Sum16 (int a, int b, int c, int d, int e, int f, int g, int h,
	                  int i, int j, int k, int l, int m, int n, int o, int p)
	{
		return a * 1 + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8
		     + i * 9 + j * 10 + k * 11 + l * 12 + m * 13 + n * 14 + o * 15 + p * 16;
	}

	public static int test_1496_sixteen_int_arguments ()
	{
		return Sum16 (Id (1), 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
	}

	class CallsAdder {
		public int bias;
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Sum (int a, int b, int c, int d, int e, int f, int g, int h, int i, int j)
		{
			return bias + a * 1 + b * 2 + c * 3 + d * 4 + e * 5
			     + f * 6 + g * 7 + h * 8 + i * 9 + j * 10;
		}
	}

	public static int test_390_instance_call_with_many_arguments ()
	{
		CallsAdder adder = new CallsAdder ();
		adder.bias = Id (5);
		return adder.Sum (1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long Mixed (sbyte a, byte b, short c, ushort d, int e, long f, float g, double h,
	                   bool i, char j, object k, CallsS12 s, IntPtr p)
	{
		long total = a + b + c + d + e + f + (long) g + (long) h;

		if (i)
			total += 1;
		total += j;
		if (k != null)
			total += 1;
		total += s.a + s.b + s.c;
		return total + (long) p;
	}

	public static int test_55_arguments_of_every_width ()
	{
		return (int) Mixed ((sbyte) Id (-1), 2, -3, 4, 5, 6, 7.0f, 8.0, true, (char) 9,
		                    IdO ("x"), MakeS12 (1, 2, 3), (IntPtr) Id (10));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void AddTo (ref int slot, int by, out int old) { old = slot; slot += by; }

	public static int test_52_ref_and_out_arguments ()
	{
		int slot = Id (2), old;

		AddTo (ref slot, Id (3), out old);
		return slot * 10 + old;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Bump12 (CallsS12 s) { s.a = 99; return s.a; }

	public static int test_1_struct_argument_is_a_copy ()
	{
		CallsS12 s = MakeS12 (Id (1), 2, 3);

		Bump12 (s);
		return s.a;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long SumS40 (CallsS40 s, int extra)
	{
		return s.a + s.b * 2 + s.c * 3 + s.d * 4 + s.e * 5 + extra;
	}

	public static int test_60_struct_passed_by_value ()
	{
		return (int) SumS40 (MakeS40 (Id (1), 2, 3, 4, 5), Id (5));
	}

	// ---------------------------------------------------------- return values

	[MethodImpl (MethodImplOptions.NoInlining)] static sbyte RetI1 (int x) { return (sbyte) x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte RetU1 (int x) { return (byte) x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static short RetI2 (int x) { return (short) x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static ushort RetU2 (int x) { return (ushort) x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long RetI8 (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static float RetR4 (double x) { return (float) x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double RetR8 (double x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static bool RetBool (int x) { return x != 0; }
	[MethodImpl (MethodImplOptions.NoInlining)] static char RetChar (int x) { return (char) x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static string RetRef (string s) { return s; }

	public static int test_1_narrow_returns_extend_and_truncate ()
	{
		return RetI1 (Id (0xff)) == -1 && RetI2 (Id (0xffff)) == -1
		       && RetU1 (Id (0x1ff)) == 255 && RetU2 (Id (0x1ffff)) == 65535
		       && RetBool (Id (7)) && RetChar (Id (65)) == 'A' ? 1 : 0;
	}

	public static int test_1_r4_return_narrows_and_r8_does_not ()
	{
		return (double) RetR4 (IdD (0.1)) != 0.1 && RetR8 (IdD (0.1)) == 0.1 ? 1 : 0;
	}

	public static int test_1_i8_and_reference_returns ()
	{
		string s = (string) IdO ("abc");

		return RetI8 (IdL (0x100000001L)) == 0x100000001L
		       && (object) RetRef (s) == (object) s ? 1 : 0;
	}

	static int void_marks;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Mark (int n) { void_marks += n; }

	public static int test_3_void_method_runs ()
	{
		Mark (Id (1));
		Mark (Id (2));
		return void_marks;
	}

	// ------------------------------------------------------- struct returns

	struct CallsS1 { public byte a; }
	struct CallsS12 { public int a, b, c; }
	struct CallsS40 { public long a, b, c, d, e; }
	struct CallsSRef { public string s; public int n; }
	struct CallsSDouble { public double x, y, z; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static CallsS1 MakeS1 (int a) { CallsS1 s; s.a = (byte) a; return s; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	static CallsS12 MakeS12 (int a, int b, int c) { CallsS12 s; s.a = a; s.b = b; s.c = c; return s; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	static CallsS40 MakeS40 (long a, long b, long c, long d, long e)
	{
		CallsS40 s; s.a = a; s.b = b; s.c = c; s.d = d; s.e = e; return s;
	}
	[MethodImpl (MethodImplOptions.NoInlining)]
	static CallsSRef MakeSRef (string s, int n) { CallsSRef v; v.s = s; v.n = n; return v; }
	[MethodImpl (MethodImplOptions.NoInlining)]
	static CallsSDouble MakeSDouble (double x, double y, double z)
	{
		CallsSDouble v; v.x = x; v.y = y; v.z = z; return v;
	}

	public static int test_7_struct_return_of_one_byte ()
	{
		return MakeS1 (Id (7)).a;
	}

	public static int test_14_struct_return_of_twelve_bytes ()
	{
		CallsS12 s = MakeS12 (Id (1), 2, 3);
		return s.a + s.b * 2 + s.c * 3;
	}

	public static int test_55_struct_return_of_forty_bytes ()
	{
		CallsS40 s = MakeS40 (IdL (1), 2, 3, 4, 5);
		return (int) (s.a + s.b * 2 + s.c * 3 + s.d * 4 + s.e * 5);
	}

	public static int test_5_struct_return_with_a_reference_field ()
	{
		CallsSRef v = MakeSRef ((string) IdO ("abcd"), Id (1));
		return v.s.Length + v.n;
	}

	public static int test_29_struct_return_of_doubles ()
	{
		CallsSDouble v = MakeSDouble (IdD (1.5), 2.5, 2.0);
		return (int) (v.x * 2 + v.y * 4 + v.z * 8);
	}

	// --------------------------------------------------- value type receivers

	struct CallsPoint {
		public int x, y;
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Sum () { return x + y; }
	}

	public static int test_5_value_type_receiver_without_constrained ()
	{
		CallsPoint p;

		p.x = Id (2);
		p.y = Id (3);
		return p.Sum ();
	}

	interface CallsIBump { void Bump (); int Value (); }

	struct CallsCounter : CallsIBump {
		public int n;
		[MethodImpl (MethodImplOptions.NoInlining)]
		public void Bump () { n++; }
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Value () { return n; }
	}

	// The receiver is the value inside the box, so the box keeps the count.
	public static int test_2_boxed_struct_interface_call_mutates_the_box ()
	{
		CallsIBump b = new CallsCounter ();

		b.Bump ();
		b.Bump ();
		return b.Value ();
	}

	public static int test_4_boxing_a_struct_copies_it ()
	{
		CallsCounter c;
		c.n = Id (4);

		CallsIBump b = c;
		b.Bump ();
		return c.n;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void BumpTwice<T> (ref T t) where T : CallsIBump { t.Bump (); t.Bump (); }

	// constrained. on a byref to a struct calls the value in place, with no box.
	public static int test_7_constrained_call_mutates_in_place ()
	{
		CallsCounter c;
		c.n = Id (5);

		BumpTwice (ref c);
		return c.n;
	}

	struct CallsScale : CallsIWeight {
		public int w;
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Weight () { return w; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int WeightOf<T> (T t) where T : CallsIWeight { return t.Weight (); }

	// The class arm dereferences the byref receiver. The struct arm overrides
	// the method, so the call lands on the value with no box.
	public static int test_15_constrained_call_on_a_class_and_on_a_struct ()
	{
		CallsScale s;
		s.w = Id (5);

		return WeightOf (new CallsHeavy ()) + WeightOf (s);
	}

	struct CallsPlainStruct { public int v; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool EqOf<T> (T a, object b) { return a.Equals (b); }

	// CallsPlainStruct does not override Equals, so the receiver has to be boxed.
	public static int test_1_constrained_call_boxes_when_not_overridden ()
	{
		CallsPlainStruct p, same, other;

		p.v = Id (5);
		same.v = Id (5);
		other.v = Id (6);
		return EqOf (p, (object) same) && !EqOf (p, (object) other) ? 1 : 0;
	}

	enum CallsColor { Red = 3, Green = 4 }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Type TypeOf<T> (T t) { return t.GetType (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int HashOf<T> (T t) { return t.GetHashCode (); }

	// A boxed Nullable<int> is a boxed int. The transform sends an enum
	// GetHashCode to the underlying type, which returns the value itself.
	public static int test_1_constrained_call_on_a_nullable_and_an_enum ()
	{
		int? v = Id (7);
		return TypeOf (v) == typeof (int) && HashOf (CallsColor.Green) == 4 ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int? NullNullable () { return null; }

	// A Nullable with no value boxes to null, so the call finds no receiver.
	public static int test_1_constrained_call_on_an_empty_nullable ()
	{
		try {
			TypeOf (NullNullable ());
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	// ---------------------------------------------------------------- inlining

	static int TinyAdd (int a, int b) { return a + b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SameAdd (int a, int b) { return a + b; }

	public static int test_1_an_inlined_callee_and_a_real_call_agree ()
	{
		int a = Id (3), b = Id (4);
		return TinyAdd (a, b) == SameAdd (a, b) ? 1 : 0;
	}

	static int GuardedDiv (int a, int b)
	{
		try {
			return a / b;
		} catch (DivideByZeroException) {
			return 7;
		}
	}

	// A callee with an exception clause is never inlined.
	public static int test_7_callee_with_a_clause_is_not_inlined ()
	{
		return GuardedDiv (Id (1), Id (0));
	}

	[MethodImpl (MethodImplOptions.AggressiveInlining)]
	static int BigAdd (int a, int b, int c, int d, int e, int f, int g, int h)
	{
		return a * 1 + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8;
	}

	// Over the length limit, so only the attribute lets this one be inlined.
	public static int test_204_aggressive_inlining ()
	{
		return BigAdd (Id (1), 2, 3, 4, 5, 6, 7, 8);
	}

	static int C0 (int x) { return x + 1; }
	static int C1 (int x) { return C0 (x) + 1; }
	static int C2 (int x) { return C1 (x) + 1; }
	static int C3 (int x) { return C2 (x) + 1; }
	static int C4 (int x) { return C3 (x) + 1; }
	static int C5 (int x) { return C4 (x) + 1; }
	static int C6 (int x) { return C5 (x) + 1; }
	static int C7 (int x) { return C6 (x) + 1; }
	static int C8 (int x) { return C7 (x) + 1; }
	static int C9 (int x) { return C8 (x) + 1; }
	static int C10 (int x) { return C9 (x) + 1; }
	static int C11 (int x) { return C10 (x) + 1; }

	// Twelve bodies, each small enough to inline and each calling the next.
	// The chain is longer than INLINE_DEPTH_LIMIT, so the innermost levels stay
	// real calls.
	public static int test_12_deep_inline_chain ()
	{
		return C11 (Id (0));
	}

	class CallsWithCctor {
		public static int seed;
		static CallsWithCctor () { seed = 5; }
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Seed () { return seed; }
	}

	public static int test_5_static_ctor_runs_before_the_call ()
	{
		return CallsWithCctor.Seed ();
	}
}
