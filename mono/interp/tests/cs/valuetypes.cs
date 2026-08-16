// box, unbox, unbox.any, initobj, ldobj, stobj and the value type copies
// between them.
//
// A NoInlining helper carries each operand to the opcode, so the transform
// cannot fold the answer and leave the opcode untested. An object that a test
// unboxes goes through VTIdO first. The transform drops a box that comes
// immediately before an unbox.any of the same type, and the call keeps that
// pair apart.
//
// Where one test covers several widths it returns one bit for each of them.

using System;
using System.Runtime.CompilerServices;

public class ValueTypes {

	[MethodImpl (MethodImplOptions.NoInlining)] static int VTId (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long VTIdL (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double VTIdD (double x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static float VTIdF (float x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static sbyte VTIdI1 (sbyte x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte VTIdU1 (byte x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static short VTIdI2 (short x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static ushort VTIdU2 (ushort x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static char VTIdC (char x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static bool VTIdBool (bool x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static IntPtr VTIdP (IntPtr x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static string VTIdStr (string x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static object VTIdO (object x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static int? VTIdN (int? x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static VTFlags VTIdFlags (VTFlags x) { return x; }

	// ldobj !!T and stobj !!T. The transform turns each into an indirect load or
	// store of the width that T instantiates to.
	[MethodImpl (MethodImplOptions.NoInlining)] static T VTLoad<T> (ref T slot) { return slot; }
	[MethodImpl (MethodImplOptions.NoInlining)] static void VTStore<T> (ref T slot, T v) { slot = v; }

	// unbox.any !!T, which for a reference type is a cast and not an unbox.
	[MethodImpl (MethodImplOptions.NoInlining)] static T VTCast<T> (object o) { return (T) o; }

	struct VTPoint { public int X, Y; }
	struct VTBig { public long A, B, C, D; }
	struct VTOdd { public byte A, B, C; }
	struct VTRefs { public string Name; public int N; }
	struct VTNest { public VTPoint P; public int Z; }
	struct VTEmpty { }
	struct VTMixed { public int? N; public VTPoint P; }

	enum VTColor { Red = 0, Green = 1, Blue = 2 }
	enum VTByteColor : byte { A = 1, B = 200 }
	enum VTLongColor : long { X = 0x100000000L }
	[Flags] enum VTFlags { None = 0, One = 1, Two = 2 }

	interface IVTBump { int Bump (); }
	struct VTCounter : IVTBump { public int N; public int Bump () { N++; return N; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static VTPoint VTMakePoint (int x, int y) { VTPoint p; p.X = x; p.Y = y; return p; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static VTBig VTMakeBig (long a, long b, long c, long d) { VTBig v; v.A = a; v.B = b; v.C = c; v.D = d; return v; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static VTOdd VTMakeOdd (byte a, byte b, byte c) { VTOdd v; v.A = a; v.B = b; v.C = c; return v; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static VTRefs VTMakeRefs (string name, int n) { VTRefs v; v.Name = name; v.N = n; return v; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int VTSumPoint (VTPoint p) { return p.X + p.Y; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long VTClobber (VTBig v) { v.A = 99; return v.A; }

	// ---------------------------------------------------------------- box

	public static int test_7_box_unbox_i4 ()
	{
		object o = VTIdO (VTId (7));
		return (int) o;
	}

	public static int test_63_box_unbox_narrow_widths ()
	{
		int r = 0;

		if ((sbyte) VTIdO (VTIdI1 (-100)) == -100) r |= 1;
		if ((byte) VTIdO (VTIdU1 (200)) == 200) r |= 2;
		if ((short) VTIdO (VTIdI2 (-30000)) == -30000) r |= 4;
		if ((ushort) VTIdO (VTIdU2 (60000)) == 60000) r |= 8;
		// The char is above 0xff, so this row covers both bytes of it.
		if ((char) VTIdO (VTIdC ('\u20ac')) == '\u20ac') r |= 16;
		if ((bool) VTIdO (VTIdBool (true))) r |= 32;
		return r;
	}

	public static int test_3_box_unbox_wide_widths ()
	{
		int r = 0;

		if ((long) VTIdO (VTIdL (0x100000003L)) == 0x100000003L) r |= 1;
		if ((IntPtr) VTIdO (VTIdP (new IntPtr (0x100000005L))) == new IntPtr (0x100000005L)) r |= 2;
		return r;
	}

	public static int test_3_box_unbox_floats ()
	{
		int r = 0;

		if ((float) VTIdO (VTIdF (1.5f)) == 1.5f) r |= 1;
		if ((double) VTIdO (VTIdD (0.1)) == 0.1) r |= 2;
		return r;
	}

	public static int test_7_box_unbox_struct ()
	{
		object o = VTIdO (VTMakePoint (VTId (3), VTId (4)));
		VTPoint p = (VTPoint) o;
		return p.X + p.Y;
	}

	public static int test_10_box_unbox_large_struct ()
	{
		object o = VTIdO (VTMakeBig (1, 2, 3, 4));
		VTBig v = (VTBig) o;
		return (int) (v.A + v.B + v.C + v.D);
	}

	public static int test_6_box_unbox_odd_size_struct ()
	{
		object o = VTIdO (VTMakeOdd (1, 2, 3));
		VTOdd v = (VTOdd) o;
		return v.A + v.B + v.C;
	}

	public static int test_1_box_unbox_struct_holding_a_reference ()
	{
		object o = VTIdO (VTMakeRefs (VTIdStr ("hello"), VTId (5)));
		VTRefs v = (VTRefs) o;
		return v.Name == "hello" && v.N == 5 ? 1 : 0;
	}

	public static int test_12_box_unbox_nested_struct ()
	{
		VTNest v;
		v.P = VTMakePoint (VTId (3), VTId (4));
		v.Z = VTId (5);
		object o = VTIdO (v);
		VTNest back = (VTNest) o;
		return back.P.X + back.P.Y + back.Z;
	}

	public static int test_1_box_unbox_struct_holding_a_nullable ()
	{
		VTMixed m;
		m.N = VTId (7);
		m.P = VTMakePoint (VTId (1), VTId (2));
		object o = VTIdO (m);
		VTMixed back = (VTMixed) o;
		return back.N == 7 && back.P.Y == 2 ? 1 : 0;
	}

	public static int test_1_box_unbox_empty_struct ()
	{
		VTEmpty e = default;
		object o = VTIdO (e);
		VTEmpty back = (VTEmpty) o;
		// A zero sized value has nothing to compare. The second box puts the
		// unboxed copy into the answer.
		object again = VTIdO (back);
		return o.GetType () == typeof (VTEmpty) && again.GetType () == typeof (VTEmpty) ? 1 : 0;
	}

	public static int test_7_box_unbox_enum_widths ()
	{
		int r = 0;

		if ((VTColor) VTIdO (VTColor.Blue) == VTColor.Blue) r |= 1;
		if ((VTByteColor) VTIdO (VTByteColor.B) == VTByteColor.B) r |= 2;
		if ((VTLongColor) VTIdO (VTLongColor.X) == VTLongColor.X) r |= 4;
		return r;
	}

	public static int test_5_box_then_unbox_any_is_a_nop ()
	{
		// The box and the unbox.any are adjacent, so the transform drops both.
		int a = VTId (5);
		return (int) (object) a;
	}

	public static int test_3_boxed_struct_is_a_copy ()
	{
		VTPoint p = VTMakePoint (VTId (3), VTId (0));
		object o = VTIdO (p);
		p.X = 50;
		return ((VTPoint) o).X;
	}

	// ------------------------------------------------------------ nullable

	public static int test_5_box_nullable_with_a_value ()
	{
		// A nullable boxes to its underlying type, so this unboxes as an int.
		object o = VTIdO (VTIdN (5));
		return (int) o;
	}

	public static int test_1_box_null_nullable_is_null ()
	{
		object o = VTIdO (VTIdN (null));
		return o == null ? 1 : 0;
	}

	public static int test_1_unbox_any_nullable_round_trip ()
	{
		object o = VTIdO (VTIdN (9));
		int? back = (int?) o;
		return back.HasValue && back.Value == 9 ? 1 : 0;
	}

	public static int test_1_unbox_any_nullable_from_null ()
	{
		object o = VTIdO (null);
		int? back = (int?) o;
		return back.HasValue ? 0 : 1;
	}

	public static int test_7_box_nullable_struct ()
	{
		VTPoint? p = VTMakePoint (VTId (3), VTId (4));
		object o = VTIdO (p);
		VTPoint q = (VTPoint) o;
		return q.X + q.Y;
	}

	public static int test_2_unbox_any_nullable_enum ()
	{
		// A nullable enum takes the UnboxExact path rather than Unbox.
		object o = VTIdO (VTColor.Blue);
		VTColor? c = (VTColor?) o;
		return (int) c.Value;
	}

	// -------------------------------------------------------------- unbox

	public static int test_7_unbox_wrong_type_throws ()
	{
		int r = 0;

		// An unbox that returns a value sets a high bit of its own. A missing
		// exception therefore cannot add up to 7.
		try { long v = (long) VTIdO (VTId (5)); r |= 8 | (int) v; } catch (InvalidCastException) { r |= 1; }
		try { VTBig v = (VTBig) VTIdO (VTMakePoint (VTId (1), VTId (2))); r |= 16 | (int) v.A; } catch (InvalidCastException) { r |= 2; }
		try { int v = (int) VTIdO (VTIdStr ("not a number")); r |= 32 | v; } catch (InvalidCastException) { r |= 4; }
		return r;
	}

	public static int test_1_unbox_null_throws ()
	{
		object o = VTIdO (null);
		try {
			return (int) o;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_4_unbox_an_enum_as_its_underlying_type ()
	{
		// Mono compares the element class, which an enum shares with the integer
		// type behind it, so both directions unbox.
		int a = (int) VTIdO (VTColor.Blue);
		VTColor b = (VTColor) VTIdO (VTId (2));
		return a + (int) b;
	}

	public static int test_3_unbox_needs_the_exact_type ()
	{
		int r = 0;

		try { uint v = (uint) VTIdO (VTId (5)); r |= 4 | (int) v; } catch (InvalidCastException) { r |= 1; }
		try { VTColor? c = (VTColor?) VTIdO (VTId (2)); r |= 8 | (c.HasValue ? 16 : 0); } catch (InvalidCastException) { r |= 2; }
		return r;
	}

	public static int test_1_unbox_any_struct_then_call ()
	{
		// unbox.any copies, so the call works on the copy and the box keeps its zero.
		object o = VTIdO (new VTCounter ());
		int n = ((VTCounter) o).Bump ();
		return n == 1 && ((VTCounter) o).N == 0 ? 1 : 0;
	}

	public static int test_2_unbox_gives_the_address_in_the_box ()
	{
		// An interface call on a boxed struct works on the box itself, so both
		// increments are still there when the box is unboxed again.
		object o = VTIdO (new VTCounter ());
		IVTBump b = (IVTBump) o;
		b.Bump ();
		b.Bump ();
		return ((VTCounter) o).N;
	}

	public static int test_7_unbox_any_reference_type ()
	{
		int r = 0;

		if (VTCast<string> (VTIdO ("abc")).Length == 3) r |= 1;
		try { VTCast<string> (VTIdO (new VTCounter ())).ToString (); } catch (InvalidCastException) { r |= 2; }
		if (VTCast<string> (VTIdO (null)) == null) r |= 4;
		return r;
	}

	// ------------------------------------------------------------- initobj

	public static int test_1_initobj_zeroes_a_large_struct ()
	{
		VTBig v = VTMakeBig (1, 2, 3, 4);
		v = default;
		return v.A == 0 && v.B == 0 && v.C == 0 && v.D == 0 ? 1 : 0;
	}

	public static int test_1_initobj_clears_a_reference_field ()
	{
		VTRefs v = VTMakeRefs (VTIdStr ("x"), VTId (1));
		v = default;
		return v.Name == null && v.N == 0 ? 1 : 0;
	}

	public static int test_1_new_struct_is_zeroed ()
	{
		// Zero is not a distinctive answer, so the sum becomes a flag.
		VTPoint p = new VTPoint ();
		return VTSumPoint (p) == 0 ? 1 : 0;
	}

	public static unsafe int test_1_initobj_through_a_pointer ()
	{
		VTPoint p = VTMakePoint (VTId (1), VTId (2));
		VTPoint* q = &p;
		*q = default;
		return p.X == 0 && p.Y == 0 ? 1 : 0;
	}

	public static unsafe int test_1_initobj_of_null_throws ()
	{
		// ECMA-335 lists no exception for initobj. The compiled engine reports a
		// null destination as a NullReferenceException.
		VTPoint* q = null;
		try {
			*q = default;
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	// ------------------------------------------------------- ldobj / stobj

	public static int test_127_ldobj_stobj_primitive_widths ()
	{
		int r = 0;

		sbyte i1 = 0; VTStore (ref i1, VTIdI1 (-5));
		if (VTLoad (ref i1) == -5) r |= 1;

		ushort u2 = 0; VTStore (ref u2, VTIdU2 (60000));
		if (VTLoad (ref u2) == 60000) r |= 2;

		int i4 = 0; VTStore (ref i4, VTId (11));
		if (VTLoad (ref i4) == 11) r |= 4;

		long i8 = 0; VTStore (ref i8, VTIdL (0x100000003L));
		if (VTLoad (ref i8) == 0x100000003L) r |= 8;

		float r4 = 0; VTStore (ref r4, VTIdF (1.5f));
		if (VTLoad (ref r4) == 1.5f) r |= 16;

		double r8 = 0; VTStore (ref r8, VTIdD (0.1));
		if (VTLoad (ref r8) == 0.1) r |= 32;

		string o = null; VTStore (ref o, VTIdStr ("hi"));
		if (VTLoad (ref o) == "hi") r |= 64;

		return r;
	}

	public static int test_7_ldobj_stobj_vt ()
	{
		VTPoint slot = default;
		VTStore (ref slot, VTMakePoint (VTId (3), VTId (4)));
		VTPoint p = VTLoad (ref slot);
		return p.X + p.Y;
	}

	public static int test_1_stobj_vt_with_a_reference ()
	{
		VTRefs slot = default;
		VTStore (ref slot, VTMakeRefs (VTIdStr ("z"), VTId (9)));
		VTRefs v = VTLoad (ref slot);
		return v.Name == "z" && v.N == 9 ? 1 : 0;
	}

	public static unsafe int test_9_stobj_ldobj_through_a_pointer ()
	{
		VTPoint p = default;
		VTPoint* q = &p;
		*q = VTMakePoint (VTId (4), VTId (5));
		VTPoint r = *q;
		return r.X + r.Y;
	}

	public static unsafe int test_1_ldobj_of_null_throws ()
	{
		int* p = null;
		try {
			return VTLoad (ref *p);
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static unsafe int test_1_ldobj_vt_of_null_throws ()
	{
		VTPoint* p = null;
		try {
			return VTLoad (ref *p).X;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static unsafe int test_1_stobj_vt_to_null_throws ()
	{
		VTPoint* p = null;
		try {
			VTStore (ref *p, VTMakePoint (1, 2));
			return 0;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	// --------------------------------------------------------- value copies

	public static int test_4_struct_assignment_copies ()
	{
		VTPoint a = VTMakePoint (VTId (1), VTId (3));
		VTPoint b = a;
		b.X = 100;
		return a.X + a.Y;
	}

	public static int test_10_large_struct_assignment_copies ()
	{
		VTBig a = VTMakeBig (1, 2, 3, 4);
		VTBig b = a;
		b.A = 100;
		return (int) (a.A + a.B + a.C + a.D);
	}

	public static int test_1_struct_argument_is_a_copy ()
	{
		VTBig a = VTMakeBig (1, 2, 3, 4);
		VTClobber (a);
		return (int) a.A;
	}

	public static int test_7_struct_array_element_copies ()
	{
		VTPoint[] arr = new VTPoint [2];
		arr [0] = VTMakePoint (VTId (3), VTId (4));
		arr [1] = arr [0];
		arr [0].X = 50;
		return arr [1].X + arr [1].Y;
	}

	public static int test_3_enum_hasflag ()
	{
		// box, ldc, box, call HasFlag is the shape the transform rewrites into an
		// integer test, and both boxes then go away.
		VTFlags f = VTIdFlags (VTFlags.One | VTFlags.Two);
		int r = 0;

		if (f.HasFlag (VTFlags.One)) r |= 1;
		if (f.HasFlag (VTFlags.Two)) r |= 2;
		if (VTIdFlags (VTFlags.One).HasFlag (VTFlags.Two)) r |= 4;
		return r;
	}
}
