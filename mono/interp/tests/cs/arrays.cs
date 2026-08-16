// The array opcodes: newarr, ldlen, ldelem and stelem in each element width,
// ldelema, and the Get/Set/Address calls a multi-dimensional array uses.
//
// Lengths and indexes come out of NoInlining helpers, so the transform cannot
// fold a bounds check away and leave the opcode untested.

using System;
using System.Runtime.CompilerServices;

public class Arrays {

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Id (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long IdL (long x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static double IdD (double x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object IdO (object x) { return x; }

	// A test that expects an allocation to throw stores it here, so its answer
	// never comes from the allocation.
	static Array ArrSink;

	struct ArrPoint {
		public int X;
		public long Y;
	}

	struct ArrTagged {
		public object Name;
		public int N;
	}

	struct ArrWide {
		public int A, B, C, D, E, F, G;
	}

	class ArrShape { public virtual int Sides () { return 0; } }
	class ArrSquare : ArrShape { public override int Sides () { return 4; } }

	enum ArrColour : byte { Red, Green, Blue }

	// ------------------------------------------------------------------
	// newarr and ldlen
	// ------------------------------------------------------------------

	public static int test_3_newarr_gives_the_length_asked_for ()
	{
		int[] a = new int[Id (3)];
		return a.Length;
	}

	public static int test_0_newarr_zeroes_the_elements ()
	{
		int[] a = new int[Id (4)];
		return a[Id (0)] + a[Id (1)] + a[Id (2)] + a[Id (3)];
	}

	public static int test_1_zero_length_array_has_no_element_zero ()
	{
		int[] a = new int[Id (0)];

		if (a.Length != 0)
			return 0;
		try {
			return a[Id (0)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	// ECMA-335 III.4.20: newarr throws OverflowException when numElems is
	// negative, not IndexOutOfRange.
	public static int test_1_newarr_refuses_a_negative_length ()
	{
		try {
			ArrSink = new int[Id (-1)];
			return 0;
		} catch (OverflowException) {
			return 1;
		}
	}

	// The length reaches newarr as a native int, so the check has to see all
	// 64 bits of it.
	public static int test_1_newarr_refuses_a_length_over_uint32 ()
	{
		try {
			ArrSink = new int[IdL (0x100000000L)];
			return 0;
		} catch (OverflowException) {
			return 1;
		}
	}

	public static int test_1_ldlen_on_null_throws ()
	{
		int[] a = (int[]) IdO (null);

		try {
			return a.Length;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_3_array_rank ()
	{
		Array a = (Array) IdO (new int[Id (2)]);
		Array b = (Array) IdO (new int[2, 2]);
		return a.Rank + b.Rank;
	}

	// ------------------------------------------------------------------
	// One element width per test
	// ------------------------------------------------------------------

	public static int test_1_ldelem_i1_sign_extends ()
	{
		sbyte[] a = new sbyte[Id (2)];
		a[Id (1)] = (sbyte) Id (-1);
		return a[Id (1)] == -1 ? 1 : 0;
	}

	public static int test_255_ldelem_u1_zero_extends ()
	{
		byte[] a = new byte[Id (2)];
		a[Id (1)] = (byte) Id (255);
		return a[Id (1)];
	}

	public static int test_1_ldelem_i2_sign_extends ()
	{
		short[] a = new short[Id (2)];
		a[Id (0)] = (short) Id (-1);
		return a[Id (0)] == -1 ? 1 : 0;
	}

	public static int test_65535_ldelem_u2_zero_extends ()
	{
		ushort[] a = new ushort[Id (2)];
		a[Id (0)] = (ushort) Id (65535);
		return a[Id (0)];
	}

	public static int test_1_ldelem_u2_on_char ()
	{
		char[] a = new char[Id (2)];
		a[Id (1)] = (char) Id (0xffff);
		return a[Id (1)] == '\uffff' ? 1 : 0;
	}

	public static int test_1_ldelem_u1_on_bool ()
	{
		bool[] a = new bool[Id (2)];
		a[Id (1)] = true;
		return a[Id (1)] && !a[Id (0)] ? 1 : 0;
	}

	public static int test_7_ldelem_i4 ()
	{
		int[] a = new int[Id (3)];
		a[Id (2)] = Id (7);
		return a[Id (2)];
	}

	public static int test_1_ldelem_u4_stays_unsigned ()
	{
		uint[] a = new uint[Id (2)];
		a[Id (0)] = (uint) Id (-1);
		return a[Id (0)] == uint.MaxValue && a[Id (0)] > int.MaxValue ? 1 : 0;
	}

	public static int test_1_ldelem_i8 ()
	{
		long[] a = new long[Id (2)];
		a[Id (1)] = IdL (0x1122334455667788L);
		return a[Id (1)] == 0x1122334455667788L ? 1 : 0;
	}

	// 0.1 has no exact float, so an element that still equals the double says
	// the array holds 8 bytes per element.
	public static int test_1_ldelem_r4_keeps_its_width ()
	{
		float[] a = new float[Id (2)];
		a[Id (1)] = (float) IdD (0.1);
		return a[Id (1)] == (float) IdD (0.1) && (double) a[Id (1)] != 0.1 ? 1 : 0;
	}

	public static int test_3_ldelem_r8 ()
	{
		double[] a = new double[Id (2)];
		a[Id (0)] = IdD (1.5);
		a[Id (1)] = IdD (1.5);
		return (int) (a[Id (0)] + a[Id (1)]);
	}

	public static int test_4_ldelem_ref ()
	{
		ArrShape[] a = new ArrShape[Id (2)];
		a[Id (1)] = new ArrSquare ();
		return a[Id (1)].Sides ();
	}

	public static unsafe int test_42_ldelem_i_on_a_pointer_array ()
	{
		int v = Id (42);
		int*[] a = new int*[Id (2)];

		a[Id (1)] = &v;
		return *a[Id (1)];
	}

	public static int test_2_ldelem_on_an_enum_array ()
	{
		ArrColour[] a = new ArrColour[Id (2)];
		a[Id (0)] = ArrColour.Blue;
		return (int) a[Id (0)];
	}

	public static int test_42_ldelem_vt ()
	{
		ArrPoint[] a = new ArrPoint[Id (2)];
		ArrPoint p;

		p.X = Id (7);
		p.Y = IdL (35);
		a[Id (1)] = p;

		ArrPoint q = a[Id (1)];
		return (int) (q.X + q.Y);
	}

	// The copy of a struct with a reference field in it has to run the write
	// barrier the collector needs.
	public static int test_1_stelem_vt_with_a_reference_field ()
	{
		ArrTagged[] a = new ArrTagged[Id (2)];
		ArrTagged t;

		t.Name = IdO ("kept");
		t.N = Id (3);
		a[Id (1)] = t;

		return (string) a[Id (1)].Name == "kept" && a[Id (1)].N == 3 ? 1 : 0;
	}

	public static int test_28_ldelem_vt_wider_than_a_stackval ()
	{
		ArrWide[] a = new ArrWide[Id (2)];
		ArrWide w;

		w.A = w.B = w.C = w.D = w.E = w.F = w.G = Id (4);
		a[Id (1)] = w;

		ArrWide r = a[Id (1)];
		return r.A + r.B + r.C + r.D + r.E + r.F + r.G;
	}

	public static int test_3_length_through_the_base_class ()
	{
		Array a = (Array) IdO (new int[Id (3)]);
		return a.Length;
	}

	// ------------------------------------------------------------------
	// Bounds, null and the covariance check
	// ------------------------------------------------------------------

	public static int test_9_last_element_is_in_range ()
	{
		int[] a = new int[Id (3)];
		a[a.Length - 1] = Id (9);
		return a[a.Length - 1];
	}

	public static int test_1_ldelem_past_the_end_throws ()
	{
		int[] a = new int[Id (3)];

		try {
			return a[Id (3)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_ldelem_at_a_negative_index_throws ()
	{
		int[] a = new int[Id (3)];

		try {
			return a[Id (-1)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_ldelem_at_int_min_throws ()
	{
		int[] a = new int[Id (3)];

		try {
			return a[Id (int.MinValue)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_stelem_past_the_end_throws ()
	{
		int[] a = new int[Id (3)];

		try {
			a[Id (3)] = Id (1);
			return 0;
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_stelem_ref_past_the_end_throws ()
	{
		object[] a = new object[Id (1)];

		try {
			a[Id (1)] = IdO ("x");
			return 0;
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_ldelem_on_null_throws ()
	{
		int[] a = (int[]) IdO (null);

		try {
			return a[Id (0)];
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_1_stelem_on_null_throws ()
	{
		int[] a = (int[]) IdO (null);

		try {
			a[Id (0)] = Id (1);
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	// The value comes out of a local rather than out of `new`, which makes this
	// a stelem instead of an ldelema and an initobj.
	public static int test_1_stelem_vt_on_null_throws ()
	{
		ArrPoint[] a = (ArrPoint[]) IdO (null);
		ArrPoint p;

		p.X = Id (1);
		p.Y = IdL (2);
		try {
			a[Id (0)] = p;
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	// ECMA-335 III.4.8 lets the index be int32 or native int. 2^32 has the same
	// low half as 0, so an engine that narrows the index instead of comparing
	// all of it answers a[0].
	public static int test_1_a_long_index_past_the_end_throws ()
	{
		int[] a = new int[Id (1)];
		long index = IdL (0x100000000L);

		try {
			return a[index];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_ldelem_vt_past_the_end_throws ()
	{
		ArrPoint[] a = new ArrPoint[Id (2)];

		try {
			ArrPoint p = a[Id (2)];
			return (int) p.Y;
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_stelem_vt_past_the_end_throws ()
	{
		ArrPoint[] a = new ArrPoint[Id (2)];
		ArrPoint p;

		p.X = Id (1);
		p.Y = IdL (2);
		try {
			a[Id (2)] = p;
			return 0;
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_stelem_ref_refuses_a_wrong_element_type ()
	{
		object[] a = new string[Id (2)];

		try {
			a[Id (0)] = new ArrSquare ();
			return 0;
		} catch (ArrayTypeMismatchException) {
			return 1;
		}
	}

	public static int test_1_stelem_ref_takes_null_whatever_the_element_type ()
	{
		object[] a = new string[Id (2)];

		a[Id (0)] = IdO ("x");
		a[Id (0)] = IdO (null);
		return a[Id (0)] == null ? 1 : 0;
	}

	public static int test_4_stelem_ref_takes_a_subclass ()
	{
		ArrShape[] a = new ArrSquare[Id (2)];

		a[Id (1)] = new ArrSquare ();
		return a[Id (1)].Sides ();
	}

	// ------------------------------------------------------------------
	// ldelema
	// ------------------------------------------------------------------

	public static int test_42_ldelema_then_store_a_field ()
	{
		ArrPoint[] a = new ArrPoint[Id (2)];

		a[Id (1)].X = Id (7);
		a[Id (1)].Y = IdL (35);
		return (int) (a[Id (1)].X + a[Id (1)].Y);
	}

	public static int test_5_ldelema_through_a_ref_local ()
	{
		ArrPoint[] a = new ArrPoint[Id (2)];
		ref ArrPoint slot = ref a[Id (0)];

		slot.X = Id (5);
		return a[Id (0)].X;
	}

	public static int test_9_ldelema_of_a_primitive ()
	{
		int[] a = new int[Id (2)];
		ref int slot = ref a[Id (1)];

		slot = Id (9);
		return a[Id (1)];
	}

	public static int test_1_ldelema_past_the_end_throws ()
	{
		ArrPoint[] a = new ArrPoint[Id (2)];

		try {
			a[Id (2)].X = Id (1);
			return 0;
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_ldelema_on_null_throws ()
	{
		ArrPoint[] a = (ArrPoint[]) IdO (null);

		try {
			a[Id (0)].X = Id (1);
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_1_ldelema_of_a_reference_element ()
	{
		object[] a = new object[Id (2)];
		ref object slot = ref a[Id (1)];

		slot = IdO ("here");
		return (string) a[Id (1)] == "here" ? 1 : 0;
	}

	// The address of an element is typed, so a string[] seen as object[]
	// refuses to hand out an object& that anything could be stored through.
	public static int test_1_ldelema_of_a_reference_element_type_checks ()
	{
		object[] a = new string[Id (2)];

		try {
			ref object slot = ref a[Id (0)];
			slot = IdO ("x");
			return 0;
		} catch (ArrayTypeMismatchException) {
			return 1;
		}
	}

	// ------------------------------------------------------------------
	// Rank 2 and rank 3
	// ------------------------------------------------------------------

	public static int test_12_rank2_length_is_the_total ()
	{
		int[,] a = new int[Id (3), Id (4)];
		return a.Length;
	}

	public static int test_7_rank2_get_and_set ()
	{
		int[,] a = new int[Id (2), Id (3)];

		a[Id (1), Id (2)] = Id (7);
		return a[Id (1), Id (2)];
	}

	public static int test_0_rank2_starts_zeroed ()
	{
		int[,] a = new int[Id (2), Id (2)];
		return a[Id (0), Id (0)] + a[Id (1), Id (1)];
	}

	public static int test_5_rank2_address ()
	{
		int[,] a = new int[Id (2), Id (2)];
		ref int slot = ref a[Id (1), Id (0)];

		slot = Id (5);
		return a[Id (1), Id (0)];
	}

	public static int test_42_rank2_of_structs ()
	{
		ArrPoint[,] a = new ArrPoint[Id (2), Id (2)];
		ArrPoint p;

		p.X = Id (7);
		p.Y = IdL (35);
		a[Id (1), Id (1)] = p;

		ArrPoint q = a[Id (1), Id (1)];
		return (int) (q.X + q.Y);
	}

	public static int test_1_rank2_out_of_range_on_the_first_index ()
	{
		int[,] a = new int[Id (2), Id (3)];

		try {
			return a[Id (2), Id (0)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_rank2_out_of_range_on_the_second_index ()
	{
		int[,] a = new int[Id (2), Id (3)];

		try {
			return a[Id (0), Id (3)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_rank2_negative_index_throws ()
	{
		int[,] a = new int[Id (2), Id (3)];

		try {
			return a[Id (0), Id (-1)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_rank2_on_null_throws ()
	{
		int[,] a = (int[,]) IdO (null);

		try {
			return a[Id (0), Id (0)];
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_1_rank2_set_refuses_a_wrong_element_type ()
	{
		object[,] a = new string[Id (1), Id (1)];

		try {
			a[Id (0), Id (0)] = new ArrSquare ();
			return 0;
		} catch (ArrayTypeMismatchException) {
			return 1;
		}
	}

	public static int test_1_rank2_address_of_a_reference_element ()
	{
		object[,] a = new object[Id (1), Id (2)];
		ref object slot = ref a[Id (0), Id (1)];

		slot = IdO ("here");
		return (string) a[Id (0), Id (1)] == "here" ? 1 : 0;
	}

	public static int test_1_rank2_with_an_empty_dimension ()
	{
		int[,] a = new int[Id (0), Id (3)];

		if (a.Length != 0)
			return 0;
		try {
			return a[Id (0), Id (0)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_rank2_new_refuses_a_negative_length ()
	{
		try {
			ArrSink = new int[Id (-1), Id (2)];
			return 0;
		} catch (OverflowException) {
			return 1;
		}
	}

	public static int test_24_rank3_length_is_the_total ()
	{
		int[,,] a = new int[Id (2), Id (3), Id (4)];
		return a.Length;
	}

	public static int test_9_rank3_get_and_set ()
	{
		int[,,] a = new int[Id (2), Id (3), Id (4)];

		a[Id (1), Id (2), Id (3)] = Id (9);
		return a[Id (1), Id (2), Id (3)];
	}

	public static int test_1_rank3_out_of_range_on_the_last_index ()
	{
		int[,,] a = new int[Id (2), Id (2), Id (2)];

		try {
			return a[Id (1), Id (1), Id (2)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_24_rank3_walks_every_element ()
	{
		int[,,] a = new int[Id (2), Id (3), Id (4)];
		int count = 0;

		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 3; j++)
				for (int k = 0; k < 4; k++)
					a[i, j, k] = Id (1);

		for (int i = 0; i < 2; i++)
			for (int j = 0; j < 3; j++)
				for (int k = 0; k < 4; k++)
					count += a[i, j, k];
		return count;
	}

	// ------------------------------------------------------------------
	// Jagged arrays
	// ------------------------------------------------------------------

	public static int test_6_jagged_array ()
	{
		int[][] a = new int[Id (3)][];
		int total = 0;

		for (int i = 0; i < a.Length; i++) {
			a[i] = new int[Id (i + 1)];
			for (int j = 0; j < a[i].Length; j++)
				a[i][j] = Id (1);
		}

		foreach (int[] row in a)
			foreach (int v in row)
				total += v;
		return total;
	}

	public static int test_1_jagged_array_row_starts_null ()
	{
		int[][] a = new int[Id (2)][];

		try {
			return a[Id (0)][Id (0)];
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_45_foreach_over_an_array ()
	{
		int[] a = new int[Id (10)];
		int total = 0;

		for (int i = 0; i < a.Length; i++)
			a[i] = i;
		foreach (int v in a)
			total += v;
		return total;
	}
}
