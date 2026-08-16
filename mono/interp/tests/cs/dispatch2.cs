// Calls that leave the interpreter through do_jit_call (), the path that
// marshals the interpreter's stack into a call made at a native entry address.
//
// The lever is MethodHandle.GetFunctionPointer (). It marks the entry address of
// a method as escaped, and resolve_code_type () answers IMETHOD_CODE_COMPILED
// for an escaped method that carries NoInlining. The target does not need a
// compiled body: at tier 0 the escaped address is the entry thunk of an
// interpreted one, and the call is marshalled all the same.
//
// Every target is NoInlining and returns a value of its own, so the answer says
// which body ran and with which arguments.

using System;
using System.Runtime.CompilerServices;

public class Dispatch2 {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long IdL (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double IdD (double x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static object IdO (object x) { return x; }

	// Gives the entry address of a method out, which marks it as escaped. Every
	// later call to that method from interpreted code goes through do_jit_call ().
	static void EscapeEntry (Type type, string name)
	{
		type.GetMethod (name).MethodHandle.GetFunctionPointer ();
	}

	// ------------------------------------------------------------- call shapes

	struct D2Pair {
		public int a, b;
	}

	// jit_call_cb () has one case per argument count, from 0 to 8. The count is
	// hasthis, plus one for a result that is not void, plus the parameter count.
	// The nine methods below reach every case.
	class D2Arity {
		public static int seen;

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static void None () { seen = 41; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Zero () { return 11; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int One (int a) { return a + 1; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Two (int a, int b) { return a * 10 + b; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Three (int a, int b, int c) { return (a * 10 + b) * 10 + c; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Four (int a, int b, int c, int d) { return a + b + c + d; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Five (int a, int b, int c, int d, int e) { return a + b + c + d + e; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Six (int a, int b, int c, int d, int e, int f)
		{
			return a + b + c + d + e + f;
		}

		[MethodImpl (MethodImplOptions.NoInlining)]
		public int SixInstance (int a, int b, int c, int d, int e, int f)
		{
			return a + b + c + d + e + f + 1;
		}

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Seven (int a, int b, int c, int d, int e, int f, int g)
		{
			return a + b + c + d + e + f + g;
		}
	}

	public static int test_41_jit_call_without_arguments_or_result ()
	{
		EscapeEntry (typeof (D2Arity), "None");
		D2Arity.seen = Id (0);
		D2Arity.None ();
		return D2Arity.seen;
	}

	public static int test_11_jit_call_without_arguments ()
	{
		EscapeEntry (typeof (D2Arity), "Zero");
		return D2Arity.Zero ();
	}

	public static int test_4_jit_call_with_one_argument ()
	{
		EscapeEntry (typeof (D2Arity), "One");
		return D2Arity.One (Id (3));
	}

	public static int test_34_jit_call_with_two_arguments ()
	{
		EscapeEntry (typeof (D2Arity), "Two");
		return D2Arity.Two (Id (3), Id (4));
	}

	public static int test_345_jit_call_with_three_arguments ()
	{
		EscapeEntry (typeof (D2Arity), "Three");
		return D2Arity.Three (Id (3), Id (4), Id (5));
	}

	public static int test_10_jit_call_with_four_arguments ()
	{
		EscapeEntry (typeof (D2Arity), "Four");
		return D2Arity.Four (Id (1), Id (2), Id (3), Id (4));
	}

	public static int test_15_jit_call_with_five_arguments ()
	{
		EscapeEntry (typeof (D2Arity), "Five");
		return D2Arity.Five (Id (1), Id (2), Id (3), Id (4), Id (5));
	}

	public static int test_21_jit_call_with_six_arguments ()
	{
		EscapeEntry (typeof (D2Arity), "Six");
		return D2Arity.Six (Id (1), Id (2), Id (3), Id (4), Id (5), Id (6));
	}

	public static int test_22_instance_jit_call_with_six_arguments ()
	{
		EscapeEntry (typeof (D2Arity), "SixInstance");
		D2Arity t = new D2Arity ();
		return t.SixInstance (Id (1), Id (2), Id (3), Id (4), Id (5), Id (6));
	}

	// A jit call marshals at most six parameters. The seventh puts this method
	// out of reach of one, so nothing uses the escaped address and the call stays
	// interpreted. resolve_code_type () prints a line about that address.
	public static int test_28_seven_arguments_is_past_the_jit_call_limit ()
	{
		EscapeEntry (typeof (D2Arity), "Seven");
		return D2Arity.Seven (Id (1), Id (2), Id (3), Id (4), Id (5), Id (6), Id (7));
	}

	// ----------------------------------------------------------- return shapes

	class D2Returns {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static sbyte Sbyte (int x) { return (sbyte) -x; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static byte Byte (int x) { return (byte) x; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static short Short (int x) { return (short) -x; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static ushort Ushort (int x) { return (ushort) x; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static long Long (long x) { return x * 1000000000L; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static double Double (double x) { return x / 2; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static string Str (int x) { return new string ('x', x); }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static D2Pair Pair (int x) { D2Pair p; p.a = x; p.b = x + 1; return p; }
	}

	// The result is one byte wide and the stack slot is wider, so do_jit_call ()
	// extends the sign of it after the call. The four narrow results below are
	// the four arms of that.
	public static int test_9_signed_byte_result_of_a_jit_call ()
	{
		EscapeEntry (typeof (D2Returns), "Sbyte");
		sbyte v = D2Returns.Sbyte (Id (9));
		return -v;
	}

	public static int test_200_unsigned_byte_result_of_a_jit_call ()
	{
		EscapeEntry (typeof (D2Returns), "Byte");
		byte v = D2Returns.Byte (Id (200));
		return v;
	}

	public static int test_5_short_result_of_a_jit_call ()
	{
		EscapeEntry (typeof (D2Returns), "Short");
		short v = D2Returns.Short (Id (5));
		return -v;
	}

	public static int test_40000_unsigned_short_result_of_a_jit_call ()
	{
		EscapeEntry (typeof (D2Returns), "Ushort");
		ushort v = D2Returns.Ushort (Id (40000));
		return v;
	}

	public static int test_3_long_result_of_a_jit_call ()
	{
		EscapeEntry (typeof (D2Returns), "Long");
		long v = D2Returns.Long (IdL (3));
		return (int) (v / 1000000000L);
	}

	public static int test_7_double_result_of_a_jit_call ()
	{
		EscapeEntry (typeof (D2Returns), "Double");
		double v = D2Returns.Double (IdD (15.0));
		return (int) v;
	}

	public static int test_6_object_result_of_a_jit_call ()
	{
		EscapeEntry (typeof (D2Returns), "Str");
		string s = D2Returns.Str (Id (6));
		return s.Length;
	}

	public static int test_7_struct_result_of_a_jit_call ()
	{
		EscapeEntry (typeof (D2Returns), "Pair");
		D2Pair p = D2Returns.Pair (Id (3));
		return p.a + p.b;
	}

	// --------------------------------------------------------- argument shapes

	struct D2Wide {
		public long a, b;
		public int c;
	}

	class D2Arguments {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static void Bump (ref int x) { x = x + 12; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Length (string s) { return s.Length; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Sum (D2Pair p) { return p.a + p.b; }

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int After (D2Wide w, int x) { return (int) (w.a + w.b) + w.c + x; }
	}

	// A byref parameter is the one kind do_jit_call () passes by value, because
	// the stack slot already holds the address.
	public static int test_12_byref_argument_of_a_jit_call ()
	{
		EscapeEntry (typeof (D2Arguments), "Bump");
		int x = Id (0);
		D2Arguments.Bump (ref x);
		return x;
	}

	public static int test_4_object_argument_of_a_jit_call ()
	{
		EscapeEntry (typeof (D2Arguments), "Length");
		return D2Arguments.Length ((string) IdO ("abcd"));
	}

	public static int test_9_struct_argument_of_a_jit_call ()
	{
		EscapeEntry (typeof (D2Arguments), "Sum");
		D2Pair p;
		p.a = Id (4);
		p.b = Id (5);
		return D2Arguments.Sum (p);
	}

	// A wide struct fills three stack slots, so the argument after it does not
	// sit at its index times the slot size. Every other test here passes
	// arguments one slot wide, where the two are the same number.
	public static int test_100_wide_struct_argument_before_a_scalar ()
	{
		EscapeEntry (typeof (D2Arguments), "After");
		D2Wide w;
		w.a = IdL (10);
		w.b = IdL (20);
		w.c = Id (30);
		return D2Arguments.After (w, Id (40));
	}

	// ------------------------------------------------- the seam under dispatch

	class D2Rank {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public virtual int Rank () { return 2; }
	}

	class D2RankChild : D2Rank {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public override int Rank () { return 7; }
	}

	interface ID2Weigh {
		int Weigh ();
	}

	class D2Scale : ID2Weigh {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Weigh () { return 5; }
	}

	// The three sites below name their target at run time, so the jit call goes
	// to the method the vtable, the interface table or the delegate gives back
	// rather than to the one the token names.
	public static int test_7_virtual_site_whose_override_takes_a_jit_call ()
	{
		EscapeEntry (typeof (D2RankChild), "Rank");
		D2Rank r = new D2RankChild ();
		return r.Rank ();
	}

	public static int test_5_interface_site_whose_target_takes_a_jit_call ()
	{
		EscapeEntry (typeof (D2Scale), "Weigh");
		ID2Weigh w = new D2Scale ();
		return w.Weigh ();
	}

	public static int test_8_delegate_over_a_jit_call_target ()
	{
		EscapeEntry (typeof (D2Arity), "One");
		Func<int, int> f = D2Arity.One;
		return f (Id (7));
	}

	// The second call answers from the JitCallInfo the first one built, so the
	// two have to agree.
	public static int test_37_two_jit_calls_to_one_target ()
	{
		EscapeEntry (typeof (D2Arity), "Two");
		return D2Arity.Two (Id (1), Id (2)) + D2Arity.Two (Id (2), Id (5));
	}

	class D2Thrower {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Boom (int x) { throw new InvalidOperationException (); }
	}

	// do_jit_call () pushes an LMF around the call. The unwinder walks back over
	// it to the interpreted frame that holds the handler.
	public static int test_1_exception_out_of_a_jit_call ()
	{
		EscapeEntry (typeof (D2Thrower), "Boom");
		try {
			return D2Thrower.Boom (Id (2));
		} catch (InvalidOperationException) {
			return 1;
		}
	}
}
