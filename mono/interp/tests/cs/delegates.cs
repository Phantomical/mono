// ldftn, ldvirtftn and the delegate call path.
//
// A method named test_<n>_<what> is a test, and it passes when it returns <n>.
// Operands go through the NoInlining Id helpers, so the transform cannot fold a
// test into its answer.

using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

public class Delegates {

	delegate int DelIntFunc (int x);
	delegate int DelNoArgs ();
	delegate void DelIntAction (int x);
	delegate long DelLongFunc (long x);
	delegate double DelDoubleFunc (double x);
	delegate float DelFloatFunc (float x);
	delegate DelPoint DelPointFunc (int x);
	delegate int DelPointArg (DelPoint p);
	delegate int DelByRef (ref int x);
	delegate int DelEight (int a, int b, int c, int d, int e, int f, int g, int h);
	delegate int DelOpenShape (DelShape shape);

	struct DelPoint {
		public int X, Y;
	}

	struct DelCounter {
		public int Value;
		public int Read () { return Value; }
	}

	class DelShape {
		public virtual int Sides () { return 4; }
		public int Corners () { return 10; }
	}

	class DelTriangle : DelShape {
		public override int Sides () { return 3; }
	}

	abstract class DelKinded {
		public abstract int Kind ();
	}

	class DelKindedNine : DelKinded {
		public override int Kind () { return 9; }
	}

	interface IDelNumbered {
		int Number ();
	}

	class DelNumbered : IDelNumbered {
		public int Number () { return 6; }
	}

	class DelBoxOf<T> {
		public static int Size () { return 8; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Id (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long IdL (long x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static double IdD (double x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object IdO (object o) { return o; }

	static int PlusOne (int x) { return x + 1; }
	static int PlusTwo (int x) { return x + 2; }
	static int PlusThree (int x) { return x + 3; }
	static int PlusFour (int x) { return x + 4; }

	static int accumulator;

	static void Accumulate (int x) { accumulator += x; }
	static void Refuse (int x) { throw new InvalidOperationException (); }

	// Each of these adds a mark of its own, so the accumulator records which
	// targets ran and in which order.
	static void Note1 (int x) { accumulator = accumulator * 10 + x; }
	static void Note2 (int x) { accumulator = accumulator * 10 + x * 2; }
	static void Note3 (int x) { accumulator = accumulator * 10 + x * 3; }

	// typeof (T) makes the answer depend on the type argument the delegate was
	// built over, which an identity target cannot show.
	static int NameLength<T> (int x) { return x + typeof (T).Name.Length; }

	static int LengthPlus (string s, int extra) { return (s == null ? 5 : s.Length) + extra; }

	static DelPoint MakePoint (int x)
	{
		DelPoint p;
		p.X = x;
		p.Y = x + 1;
		return p;
	}

	static int PointSum (DelPoint p) { return p.X + p.Y; }

	static int BumpRef (ref int x) { x += 3; return x; }

	// The weights make the answer depend on which slot each argument arrived in.
	static int WeighEight (int a, int b, int c, int d, int e, int f, int g, int h)
	{
		return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8;
	}

	static long DoubleLong (long x) { return x * 2; }
	static double HalfDouble (double x) { return x / 2; }
	static float HalfFloat (float x) { return x / 2; }

	static int ArityZero () { return 1; }
	static int ArityOne (int a) { return a + 1; }
	static int ArityTwo (int a, int b) { return a + b * 2; }
	static int ArityThree (int a, int b, int c) { return a + b * 2 + c * 4; }

	[DllImport ("__Internal", EntryPoint = "interp_test_abs")]
	static extern int NativeAbs (int value);

	public static int test_7_static_target ()
	{
		DelIntFunc d = PlusFour;
		return d (Id (3));
	}

	public static int test_10_instance_target ()
	{
		DelShape shape = new DelShape ();
		DelNoArgs d = shape.Corners;
		return d ();
	}

	public static int test_3_virtual_target ()
	{
		DelShape shape = new DelTriangle ();
		DelNoArgs d = shape.Sides;
		return d ();
	}

	public static int test_4_virtual_target_on_the_base_class ()
	{
		DelShape shape = new DelShape ();
		DelNoArgs d = shape.Sides;
		return d ();
	}

	public static int test_6_interface_target ()
	{
		IDelNumbered n = new DelNumbered ();
		DelNoArgs d = n.Number;
		return d ();
	}

	public static int test_9_abstract_target_binds_the_override ()
	{
		MethodInfo m = typeof (DelKinded).GetMethod ("Kind");
		DelNoArgs d = (DelNoArgs) Delegate.CreateDelegate (typeof (DelNoArgs), new DelKindedNine (), m);
		return d ();
	}

	public static int test_7_open_instance_delegate_picks_the_receiver ()
	{
		// The delegate has no target, so each call resolves Sides again on the
		// receiver it is given.
		MethodInfo m = typeof (DelShape).GetMethod ("Sides");
		DelOpenShape d = (DelOpenShape) Delegate.CreateDelegate (typeof (DelOpenShape), null, m);
		return d (new DelTriangle ()) + d (new DelShape ());
	}

	public static int test_1_null_delegate_throws ()
	{
		DelIntFunc d = null;

		try {
			return d (Id (3));
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_1_null_receiver_throws_at_ldvirtftn ()
	{
		DelShape shape = (DelShape) IdO (null);
		int made = 0;

		try {
			DelNoArgs d = shape.Sides;
			made = 1;
			return d ();
		} catch (NullReferenceException) {
			// made is still 0 only if the load of the method threw. A
			// throw from the call itself is a different defect.
			return made == 0 ? 1 : 0;
		}
	}

	public static int test_9_multicast_returns_the_last_result ()
	{
		DelIntFunc d = PlusOne;
		d += PlusTwo;
		d += PlusThree;
		return d (Id (6));
	}

	public static int test_6_multicast_runs_every_target ()
	{
		accumulator = 0;
		DelIntAction d = Accumulate;
		d += Accumulate;
		d += Accumulate;
		d (Id (2));
		return accumulator;
	}

	public static int test_3_invocation_list_length ()
	{
		DelIntFunc d = PlusOne;
		d += PlusTwo;
		d += PlusThree;
		return d.GetInvocationList ().Length;
	}

	public static int test_13_remove_from_the_middle ()
	{
		accumulator = 0;
		DelIntAction d = Note1;
		d += Note2;
		d += Note3;
		d -= Note2;
		d (Id (1));
		return accumulator;
	}

	public static int test_1_remove_the_only_target_gives_null ()
	{
		DelIntFunc d = PlusOne;
		d -= PlusOne;
		return d == null ? 1 : 0;
	}

	public static int test_7_combine_with_null_is_the_other_one ()
	{
		DelIntFunc d = PlusFour;
		DelIntFunc combined = (DelIntFunc) Delegate.Combine (null, d);
		return combined (Id (3));
	}

	public static int test_2_multicast_stops_at_a_throwing_target ()
	{
		accumulator = 0;
		DelIntAction d = Accumulate;
		d += Refuse;
		d += Accumulate;

		try {
			d (Id (1));
		} catch (InvalidOperationException) {
			return accumulator + 1;
		}
		return 0;
	}

	public static int test_4_multicast_of_two_multicasts ()
	{
		accumulator = 0;
		DelIntAction a = Accumulate;
		a += Accumulate;
		DelIntAction b = Accumulate;
		b += Accumulate;
		DelIntAction both = a + b;
		both (Id (1));
		return accumulator;
	}

	public static int test_11_struct_return ()
	{
		DelPointFunc d = MakePoint;
		DelPoint p = d (Id (5));
		return p.X + p.Y;
	}

	public static int test_11_struct_argument ()
	{
		DelPointArg d = PointSum;
		return d (MakePoint (Id (5)));
	}

	public static int test_8_byref_argument ()
	{
		int v = Id (5);
		DelByRef d = BumpRef;
		d (ref v);
		return v;
	}

	public static int test_204_eight_arguments ()
	{
		DelEight d = WeighEight;
		return d (Id (1), 2, Id (3), 4, 5, Id (6), 7, 8);
	}

	public static int test_6_long_return ()
	{
		DelLongFunc d = DoubleLong;
		return (int) d (IdL (3));
	}

	public static int test_3_double_return ()
	{
		DelDoubleFunc d = HalfDouble;
		return (int) d (IdD (7.0));
	}

	public static int test_1_float_argument_and_return ()
	{
		DelFloatFunc d = HalfFloat;
		return d ((float) IdD (0.1)) == 0.05f ? 1 : 0;
	}

	public static int test_7_boxed_struct_target ()
	{
		DelCounter c = new DelCounter ();
		c.Value = Id (7);
		DelNoArgs d = c.Read;
		return d ();
	}

	public static int test_7_boxed_struct_target_is_a_copy ()
	{
		DelCounter c = new DelCounter ();
		c.Value = Id (7);
		DelNoArgs d = c.Read;
		c.Value = Id (99);
		return d ();
	}

	public static int test_9_static_target_with_a_bound_argument ()
	{
		// The target is static and takes one argument more than the delegate.
		// The target object becomes that first argument.
		MethodInfo m = typeof (Delegates).GetMethod ("LengthPlus", BindingFlags.Static | BindingFlags.NonPublic);
		DelIntFunc d = (DelIntFunc) Delegate.CreateDelegate (typeof (DelIntFunc), "abcdef", m);
		return d (Id (3));
	}

	public static int test_8_static_target_bound_to_null ()
	{
		MethodInfo m = typeof (Delegates).GetMethod ("LengthPlus", BindingFlags.Static | BindingFlags.NonPublic);
		DelIntFunc d = (DelIntFunc) Delegate.CreateDelegate (typeof (DelIntFunc), null, m);
		return d (Id (3));
	}

	public static int test_5_dynamic_invoke ()
	{
		DelIntFunc d = PlusFour;
		return (int) d.DynamicInvoke (new object [] { Id (1) });
	}

	public static int test_9_dynamic_invoke_on_a_multicast ()
	{
		DelIntFunc d = PlusOne;
		d += PlusTwo;
		d += PlusThree;
		return (int) d.DynamicInvoke (new object [] { Id (6) });
	}

	public static int test_1_dynamic_invoke_wraps_the_exception ()
	{
		DelIntAction d = Refuse;

		try {
			d.DynamicInvoke (new object [] { Id (1) });
		} catch (TargetInvocationException e) {
			return e.InnerException is InvalidOperationException ? 1 : 0;
		}
		return 0;
	}

	public static int test_1_method_property_names_the_target ()
	{
		DelIntFunc d = PlusFour;
		return d.Method.Name == "PlusFour" ? 1 : 0;
	}

	public static int test_1_target_property_is_the_receiver ()
	{
		DelShape shape = new DelShape ();
		DelNoArgs d = shape.Corners;
		return (object) d.Target == (object) shape ? 1 : 0;
	}

	public static int test_1_a_static_target_has_no_receiver ()
	{
		DelIntFunc d = PlusFour;
		return d.Target == null ? 1 : 0;
	}

	public static int test_1_delegates_over_the_same_target_are_equal ()
	{
		DelShape shape = new DelShape (), other = new DelShape ();
		DelNoArgs a = shape.Corners;
		DelNoArgs b = shape.Corners;
		DelNoArgs elsewhere = other.Corners;
		return a.Equals (b) && !a.Equals (elsewhere) ? 1 : 0;
	}

	public static int test_5_lambda_capturing_a_local ()
	{
		int captured = Id (2);
		Func<int, int> f = v => v + captured;
		return f (Id (3));
	}

	public static int test_4_lambda_without_a_capture ()
	{
		Func<int, int> f = v => v * 2;
		return f (Id (2));
	}

	public static int test_19_func_arities ()
	{
		Func<int> f0 = ArityZero;
		Func<int, int> f1 = ArityOne;
		Func<int, int, int> f2 = ArityTwo;
		Func<int, int, int, int> f3 = ArityThree;
		return f0 () + f1 (Id (1)) + f2 (Id (1), 2) + f3 (Id (1), 1, 2);
	}

	public static int test_8_action_arities ()
	{
		accumulator = 0;
		Action a1 = () => accumulator += 1;
		Action<int> a2 = v => accumulator += v;
		Action<int, int> a3 = (v, w) => accumulator += v + w * 2;
		a1 ();
		a2 (Id (2));
		a3 (Id (1), 2);
		return accumulator;
	}

	public static int test_7_delegate_over_another_delegates_invoke ()
	{
		DelIntFunc inner = PlusFour;
		DelIntFunc outer = inner.Invoke;
		return outer (Id (3));
	}

	public static int test_8_generic_method_target ()
	{
		DelIntFunc d = NameLength<int>;
		return d (Id (3));
	}

	public static int test_9_generic_method_target_over_a_reference_type ()
	{
		DelIntFunc d = NameLength<string>;
		return d (Id (3));
	}

	public static int test_8_static_target_in_a_generic_class ()
	{
		DelNoArgs d = DelBoxOf<string>.Size;
		return d ();
	}

	public static int test_5_delegate_from_an_array_slot ()
	{
		DelIntFunc [] table = { PlusOne, PlusTwo };
		return table [Id (0)] (Id (4));
	}

	public static int test_9_delegate_passed_as_an_argument ()
	{
		return ApplyTwice (PlusOne, Id (7));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ApplyTwice (DelIntFunc d, int x)
	{
		return d (d (x));
	}

	public static int test_7_pinvoke_target ()
	{
		DelIntFunc d = NativeAbs;
		return d (Id (-7));
	}
}
