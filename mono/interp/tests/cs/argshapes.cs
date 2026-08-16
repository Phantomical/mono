// How a call lays out its argument area: many arguments, arguments of mixed
// width, value types passed by value at several sizes, byrefs among by-value
// arguments, and the same shape called with and without a receiver.
//
// A method named test_<n>_<what> is a test. It passes when it returns <n>.
// A callee with more than a few arguments counts the arguments that hold the
// value the caller sent, and returns that count. A wrong answer then says how
// many slots arrived correctly. No two arguments carry the same value: a
// checksum that two exchanged slots agree on tests nothing.
//
// The hot group calls one callee often enough for a promotion to land while the
// caller keeps interpreting. The calls after that leave the interpreter through
// the jit-call marshalling, which lays the arguments out from an offset table of
// its own. Only the promotion arm runs that path: with no promotion the call
// stays interpreted, and with no interpreter there is no interpreted caller.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

public class ArgShapes {

	[MethodImpl (MethodImplOptions.NoInlining)] static int ASId (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long ASIdL (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static float ASIdF (float x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte ASIdU1 (byte x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static object ASIdO (object x) { return x; }

	struct ASPair { public int A, B; }
	struct ASQuad { public long A, B; }
	struct ASBig24 { public long A, B, C; }
	struct ASBig40 { public long A, B, C, D, E; }
	struct ASOdd3 { public byte A, B, C; }
	struct ASRefs { public string S; public int N; }
	struct ASNest { public ASPair P; public int Z; }
	struct ASFloats { public float F; public double D; }
	struct ASEmpty { }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static ASPair MakePair (int a, int b) { ASPair p; p.A = a; p.B = b; return p; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static ASQuad MakeQuad (long a, long b) { ASQuad q; q.A = a; q.B = b; return q; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static ASBig24 MakeBig24 (long a, long b, long c)
	{
		ASBig24 s; s.A = a; s.B = b; s.C = c; return s;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static ASBig40 MakeBig40 (long a, long b, long c, long d, long e)
	{
		ASBig40 s; s.A = a; s.B = b; s.C = c; s.D = d; s.E = e; return s;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static ASOdd3 MakeOdd3 (byte a, byte b, byte c)
	{
		ASOdd3 s; s.A = a; s.B = b; s.C = c; return s;
	}

	// ------------------------------------------------------------ arity ladder

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASOne (int a) { return a == 11 ? 1 : 0; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASEight (int a, long b, int c, long d, float e, double f, char g, bool h)
	{
		int n = 0;
		n += a == 1 ? 1 : 0;
		n += b == 0x200000002L ? 1 : 0;
		n += c == 3 ? 1 : 0;
		n += d == 0x400000004L ? 1 : 0;
		n += e == 5.5f ? 1 : 0;
		n += f == 6.25 ? 1 : 0;
		n += g == 'g' ? 1 : 0;
		n += h ? 1 : 0;
		return n;
	}

	public static int test_8_eight_arguments_of_mixed_width ()
	{
		return ASEight (ASId (1), 0x200000002L, 3, 0x400000004L, 5.5f, 6.25, 'g', true);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASEightNarrow (sbyte a, byte b, short c, ushort d,
	                          char e, bool f, sbyte g, byte h)
	{
		int n = 0;
		n += a == -1 ? 1 : 0;
		n += b == 2 ? 1 : 0;
		n += c == -3 ? 1 : 0;
		n += d == 4 ? 1 : 0;
		n += e == (char) 5 ? 1 : 0;
		// The false one fails on any wide value that lands here; the true one in
		// ASEight fails on a slot that arrives zeroed.
		n += !f ? 1 : 0;
		n += g == -7 ? 1 : 0;
		n += h == 8 ? 1 : 0;
		return n;
	}

	// Every one of these is narrower than a slot, and each still gets a slot.
	public static int test_8_eight_narrow_arguments ()
	{
		return ASEightNarrow ((sbyte) ASId (-1), 2, -3, 4, (char) 5, false, -7, 8);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASNarrowWide (byte a, long b, byte c, double d, byte e, long f)
	{
		int n = 0;
		n += a == 1 ? 1 : 0;
		n += b == 0x200000002L ? 1 : 0;
		n += c == 3 ? 1 : 0;
		n += d == 4.5 ? 1 : 0;
		n += e == 5 ? 1 : 0;
		n += f == 0x600000006L ? 1 : 0;
		return n;
	}

	public static int test_6_narrow_and_wide_alternate ()
	{
		return ASNarrowWide (ASIdU1 (1), 0x200000002L, 3, 4.5, 5, 0x600000006L);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASSixteen (int a, long b, int c, long d, int e, long f, int g, long h,
	                      int i, long j, int k, long l, int m, long n, int o, long p)
	{
		int hits = 0;
		hits += a == 1 ? 1 : 0;
		hits += b == 2 ? 1 : 0;
		hits += c == 3 ? 1 : 0;
		hits += d == 4 ? 1 : 0;
		hits += e == 5 ? 1 : 0;
		hits += f == 6 ? 1 : 0;
		hits += g == 7 ? 1 : 0;
		hits += h == 8 ? 1 : 0;
		hits += i == 9 ? 1 : 0;
		hits += j == 10 ? 1 : 0;
		hits += k == 11 ? 1 : 0;
		hits += l == 12 ? 1 : 0;
		hits += m == 13 ? 1 : 0;
		hits += n == 14 ? 1 : 0;
		hits += o == 15 ? 1 : 0;
		hits += p == 16 ? 1 : 0;
		return hits;
	}

	// Sixteen arguments of one width give the same stride at every index. These
	// alternate, so an index computed from the first argument's width is wrong
	// from the second one on.
	public static int test_16_sixteen_arguments_alternate_width ()
	{
		return ASSixteen (ASId (1), 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASTwentyFour (int a1, long a2, byte a3, double a4, short a5, float a6,
	                         int a7, long a8, char a9, bool a10, sbyte a11, double a12,
	                         ASPair a13, ushort a14, long a15, float a16,
	                         int a17, string a18, long a19, ASOdd3 a20,
	                         double a21, int a22, ASQuad a23, object a24)
	{
		int n = 0;
		n += a1 == 1 ? 1 : 0;
		n += a2 == 2 ? 1 : 0;
		n += a3 == 3 ? 1 : 0;
		n += a4 == 4.5 ? 1 : 0;
		n += a5 == -5 ? 1 : 0;
		n += a6 == 6.5f ? 1 : 0;
		n += a7 == 7 ? 1 : 0;
		n += a8 == 8 ? 1 : 0;
		n += a9 == (char) 9 ? 1 : 0;
		n += a10 ? 1 : 0;
		n += a11 == -11 ? 1 : 0;
		n += a12 == 12.5 ? 1 : 0;
		n += a13.A == 13 && a13.B == 130 ? 1 : 0;
		n += a14 == 14 ? 1 : 0;
		n += a15 == 15 ? 1 : 0;
		n += a16 == 16.5f ? 1 : 0;
		n += a17 == 17 ? 1 : 0;
		n += a18 == "eighteen" ? 1 : 0;
		n += a19 == 19 ? 1 : 0;
		n += a20.A == 20 && a20.B == 201 && a20.C == 202 ? 1 : 0;
		n += a21 == 21.5 ? 1 : 0;
		n += a22 == 22 ? 1 : 0;
		n += a23.A == 23 && a23.B == 230 ? 1 : 0;
		n += (a24 as string) == "twentyfour" ? 1 : 0;
		return n;
	}

	public static int test_24_twenty_four_arguments ()
	{
		return ASTwentyFour (ASId (1), 2, 3, 4.5, -5, 6.5f, 7, 8, (char) 9, true, -11, 12.5,
		                     MakePair (13, 130), 14, 15, 16.5f,
		                     17, "eighteen", 19, MakeOdd3 (20, 201, 202),
		                     21.5, 22, MakeQuad (23, 230), "twentyfour");
	}

	// -------------------------------------------------------------- value types

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASPairBetween (int a, ASPair p, int b)
	{
		int n = 0;
		n += a == 1 ? 1 : 0;
		n += p.A == 2 && p.B == 3 ? 1 : 0;
		n += b == 4 ? 1 : 0;
		return n;
	}

	public static int test_3_small_struct_between_integers ()
	{
		return ASPairBetween (ASId (1), MakePair (2, 3), 4);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASQuadThenInt (ASQuad q, int tail)
	{
		int n = 0;
		n += q.A == 100 ? 1 : 0;
		n += q.B == 200 ? 1 : 0;
		n += tail == 7 ? 1 : 0;
		return n;
	}

	public static int test_3_two_word_struct_then_an_integer ()
	{
		return ASQuadThenInt (MakeQuad (ASIdL (100), 200), 7);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASAfterBig40 (ASBig40 s, int tail)
	{
		int n = 0;
		n += s.A == 1 ? 1 : 0;
		n += s.B == 2 ? 1 : 0;
		n += s.C == 3 ? 1 : 0;
		n += s.D == 4 ? 1 : 0;
		n += s.E == 5 ? 1 : 0;
		n += tail == 7 ? 1 : 0;
		return n;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASTwoBigStructs (ASBig40 a, ASBig24 b, int tail)
	{
		int n = 0;
		n += a.A == 1 && a.E == 5 ? 1 : 0;
		n += b.A == 10 && b.C == 30 ? 1 : 0;
		n += tail == 9 ? 1 : 0;
		return n;
	}

	public static int test_3_two_large_structs_and_a_tail ()
	{
		return ASTwoBigStructs (MakeBig40 (ASIdL (1), 2, 3, 4, 5),
		                        MakeBig24 (10, 20, 30), 9);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASOdd3ThenInt (ASOdd3 s, int tail, ASOdd3 t)
	{
		int n = 0;
		n += s.A == 1 && s.B == 2 && s.C == 3 ? 1 : 0;
		n += tail == 4 ? 1 : 0;
		n += t.A == 5 && t.B == 6 && t.C == 7 ? 1 : 0;
		return n;
	}

	// Three bytes of value in a slot of eight, twice, with an int between them.
	public static int test_3_odd_sized_structs_around_an_integer ()
	{
		return ASOdd3ThenInt (MakeOdd3 (ASIdU1 (1), 2, 3), 4, MakeOdd3 (5, 6, 7));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASRefsAmongInts (int a, ASRefs r, long b, string s)
	{
		int n = 0;
		n += a == 1 ? 1 : 0;
		n += r.S == "held" && r.N == 2 ? 1 : 0;
		n += b == 3 ? 1 : 0;
		n += s == "plain" ? 1 : 0;
		return n;
	}

	public static int test_4_struct_holding_a_reference ()
	{
		ASRefs r;
		r.S = (string) ASIdO ("held");
		r.N = 2;
		return ASRefsAmongInts (ASId (1), r, 3, "plain");
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASNestArg (ASNest a, int tail)
	{
		int n = 0;
		n += a.P.A == 1 && a.P.B == 2 ? 1 : 0;
		n += a.Z == 3 ? 1 : 0;
		n += tail == 4 ? 1 : 0;
		return n;
	}

	public static int test_3_nested_struct_argument ()
	{
		ASNest a;
		a.P = MakePair (ASId (1), 2);
		a.Z = 3;
		return ASNestArg (a, 4);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASEmptyBetween (int a, ASEmpty e, int b)
	{
		int n = 0;
		n += a == 5 ? 1 : 0;
		n += b == 6 ? 1 : 0;
		return n;
	}

	// A struct with no fields is one byte wide, so it still takes a slot of its
	// own and the argument behind it moves.
	public static int test_2_empty_struct_between_integers ()
	{
		ASEmpty e = new ASEmpty ();
		return ASEmptyBetween (ASId (5), e, 6);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASFloatStruct (ASFloats f, float tail)
	{
		int n = 0;
		n += f.F == 1.5f ? 1 : 0;
		n += f.D == 2.25 ? 1 : 0;
		n += tail == 3.5f ? 1 : 0;
		return n;
	}

	public static int test_3_struct_of_a_float_and_a_double ()
	{
		ASFloats f;
		f.F = ASIdF (1.5f);
		f.D = 2.25;
		return ASFloatStruct (f, 3.5f);
	}

	// --------------------------------------------------------------- byrefs

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASByrefAmongValues (int a, ref long b, double c, ref ASPair d, int e)
	{
		int n = 0;
		n += a == 1 ? 1 : 0;
		n += b == 2 ? 1 : 0;
		n += c == 3.5 ? 1 : 0;
		n += d.A == 4 && d.B == 5 ? 1 : 0;
		n += e == 6 ? 1 : 0;
		b = 20;
		d.A = 40;
		return n;
	}

	public static int test_7_byref_among_by_value ()
	{
		long b = ASIdL (2);
		ASPair d = MakePair (4, 5);
		int n = ASByrefAmongValues (ASId (1), ref b, 3.5, ref d, 6);

		return n + (b == 20 ? 1 : 0) + (d.A == 40 ? 1 : 0);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASOutAmongValues (int a, out long b, ASPair c, out ASPair d, long e)
	{
		b = 20;
		d = MakePair (40, 41);
		int n = 0;
		n += a == 1 ? 1 : 0;
		n += c.A == 3 && c.B == 30 ? 1 : 0;
		n += e == 5 ? 1 : 0;
		return n;
	}

	public static int test_5_out_parameters_among_by_value ()
	{
		long b;
		ASPair d;
		int n = ASOutAmongValues (ASId (1), out b, MakePair (3, 30), out d, 5);

		return n + (b == 20 ? 1 : 0) + (d.A == 40 && d.B == 41 ? 1 : 0);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASByrefOfEveryWidth (ref sbyte a, ref short b, ref int c,
	                                ref long d, ref float e, ref double f)
	{
		int n = 0;
		n += a == -1 ? 1 : 0;
		n += b == -2 ? 1 : 0;
		n += c == 3 ? 1 : 0;
		n += d == 4 ? 1 : 0;
		n += e == 5.5f ? 1 : 0;
		n += f == 6.25 ? 1 : 0;
		a = -10;
		f = 60.5;
		return n;
	}

	// A byref is one slot whatever it points at, so the width of the target does
	// not change where the argument behind it goes.
	public static int test_8_byref_of_every_width ()
	{
		sbyte a = (sbyte) ASId (-1);
		short b = -2;
		int c = 3;
		long d = 4;
		float e = 5.5f;
		double f = 6.25;
		int n = ASByrefOfEveryWidth (ref a, ref b, ref c, ref d, ref e, ref f);

		return n + (a == -10 ? 1 : 0) + (f == 60.5 ? 1 : 0);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASRefAndCopy (ref ASBig40 a, ASBig40 b)
	{
		a.A = 100;
		return b.A == 1 && b.E == 5 ? 1 : 0;
	}

	// The by-value copy is taken at the call site, so the write through the byref
	// must not reach it.
	public static int test_2_a_struct_by_reference_and_the_same_by_value ()
	{
		ASBig40 s = MakeBig40 (ASIdL (1), 2, 3, 4, 5);
		int n = ASRefAndCopy (ref s, s);

		return n + (s.A == 100 ? 1 : 0);
	}

	// ---------------------------------------------------------- floating point

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASFloatBetweenInts (int a, float b, int c, double d, int e)
	{
		int n = 0;
		n += a == 1 ? 1 : 0;
		n += b == 2.5f ? 1 : 0;
		n += c == 3 ? 1 : 0;
		n += d == 4.25 ? 1 : 0;
		n += e == 5 ? 1 : 0;
		return n;
	}

	public static int test_5_a_float_and_a_double_between_integers ()
	{
		return ASFloatBetweenInts (ASId (1), 2.5f, 3, 4.25, 5);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASFloatsAmongLongs (long a, float b, long c, float d, double e, long f)
	{
		int n = 0;
		n += a == 0x100000001L ? 1 : 0;
		n += b == 2.5f ? 1 : 0;
		n += c == 0x300000003L ? 1 : 0;
		n += d == 4.5f ? 1 : 0;
		n += e == 5.25 ? 1 : 0;
		n += f == 0x600000006L ? 1 : 0;
		return n;
	}

	// A float holds four bytes in a slot of eight, and the long behind it must
	// not read the half the float did not write.
	public static int test_6_floats_among_longs ()
	{
		return ASFloatsAmongLongs (ASIdL (0x100000001L), 2.5f, 0x300000003L,
		                           4.5f, 5.25, 0x600000006L);
	}

	// ------------------------------------------------------------- a receiver

	class ASHolder {
		public int Bias;

		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Take (int a, long b, double c, ASPair d)
		{
			int n = 0;
			n += Bias == 9 ? 1 : 0;
			n += a == 1 ? 1 : 0;
			n += b == 2 ? 1 : 0;
			n += c == 3.5 ? 1 : 0;
			n += d.A == 4 && d.B == 5 ? 1 : 0;
			return n;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ASTakeStatic (int bias, int a, long b, double c, ASPair d)
	{
		int n = 0;
		n += bias == 9 ? 1 : 0;
		n += a == 1 ? 1 : 0;
		n += b == 2 ? 1 : 0;
		n += c == 3.5 ? 1 : 0;
		n += d.A == 4 && d.B == 5 ? 1 : 0;
		return n;
	}

	// The receiver takes the slot the static form gives to its first argument.
	public static int test_2_an_instance_and_a_static_of_one_shape ()
	{
		ASHolder h = new ASHolder ();
		h.Bias = ASId (9);
		ASPair d = MakePair (4, 5);
		int instance = h.Take (1, 2, 3.5, d);
		int stat = ASTakeStatic (9, 1, 2, 3.5, d);

		return (instance == 5 ? 1 : 0) + (stat == 5 ? 1 : 0);
	}

	struct ASCounter {
		public int Bias;

		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Take (int a, ASBig24 b, long c)
		{
			int n = 0;
			n += Bias == 7 ? 1 : 0;
			n += a == 1 ? 1 : 0;
			n += b.A == 10 && b.C == 30 ? 1 : 0;
			n += c == 3 ? 1 : 0;
			Bias = 70;
			return n;
		}
	}

	// The receiver of a value type is a byref, so it is one slot however wide
	// the value is, and a write through it reaches the caller's copy.
	public static int test_5_a_value_type_receiver ()
	{
		ASCounter c;
		c.Bias = ASId (7);
		int n = c.Take (1, MakeBig24 (10, 20, 30), 3);

		return n + (c.Bias == 70 ? 1 : 0);
	}

	// ------------------------------------------------------------- the hot set
	//
	// The four shapes here are what the jit-call offset table is built from: a
	// single argument, arguments of mixed width, an argument wider than a slot,
	// and a receiver. The count buys the wall time a background compile needs.
	// A round is a few hundred nanoseconds, so a loop is tens of milliseconds.

	const int HotRounds = 100000;

	public static int test_1_hot_one_argument_callee ()
	{
		int ok = 0;
		for (int i = 0; i < HotRounds; i++)
			ok += ASOne (ASId (11));
		return ok == HotRounds ? 1 : 0;
	}

	public static int test_1_hot_narrow_argument_callee ()
	{
		int ok = 0;
		for (int i = 0; i < HotRounds; i++)
			ok += ASNarrowWide (ASIdU1 (1), 0x200000002L, 3, 4.5, 5, 0x600000006L) == 6 ? 1 : 0;
		return ok == HotRounds ? 1 : 0;
	}

	public static int test_1_hot_struct_argument_callee ()
	{
		ASBig40 s = MakeBig40 (ASIdL (1), 2, 3, 4, 5);
		int ok = 0;
		for (int i = 0; i < HotRounds; i++)
			ok += ASAfterBig40 (s, 7) == 6 ? 1 : 0;
		return ok == HotRounds ? 1 : 0;
	}

	public static int test_1_hot_instance_callee ()
	{
		ASHolder h = new ASHolder ();
		h.Bias = ASId (9);
		ASPair d = MakePair (4, 5);
		int ok = 0;
		for (int i = 0; i < HotRounds; i++)
			ok += h.Take (1, 2, 3.5, d) == 5 ? 1 : 0;
		return ok == HotRounds ? 1 : 0;
	}

	// -------------------------------------------------------- across a wrapper

	// A caller puts the address a large value type comes back at in the first
	// argument register. The callee writes through that same register, so the
	// result of this call is 24 bytes of the fill value.
	[DllImport ("__Internal", EntryPoint = "interp_test_memset")]
	static extern ASBig24 NativeFillBig24 (int fill, IntPtr count);

	public static int test_1_a_large_struct_returned_through_a_pointer ()
	{
		ASBig24 s = NativeFillBig24 (ASId (0x41), (IntPtr) 24);
		const long All = 0x4141414141414141L;

		return s.A == All && s.B == All && s.C == All ? 1 : 0;
	}
}
