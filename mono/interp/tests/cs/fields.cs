// Field access: ldfld, stfld, ldflda, ldsfld, stsfld and ldsflda, on each width
// the interpreter has an opcode for, and on the static, thread-static and
// remoted forms of a field.
//
// Values come out of the Id helpers and objects out of the Make helpers, all of
// them NoInlining, so the transform cannot fold a field access into a constant.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

struct Fields_Point {
	public int X;
	public int Y;

	public Fields_Point (int x, int y)
	{
		X = x;
		Y = y;
	}
}

// One field per scalar width, so a load off a value type on the stack has a
// target for each of the scalar MINT_LDFLD_VT_* opcodes. Fields_Outer carries
// the nested case, MINT_LDFLD_VT_VT.
struct Fields_Mixed {
	public sbyte I1;
	public byte U1;
	public short I2;
	public ushort U2;
	public int I4;
	public long I8;
	public float R4;
	public double R8;
	public string O;
}

struct Fields_Inner {
	public int X;
	public long Y;
}

struct Fields_Outer {
	public int Head;
	public Fields_Inner Inner;
}

struct Fields_Tagged {
	public string Name;
	public int N;
}

struct Fields_Counter {
	public int A;
	public long B;

	[MethodImpl (MethodImplOptions.NoInlining)]
	public void Add (int n)
	{
		A += n;
		B += n;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public int Total () { return A + (int) B; }
}

class Fields_Cell {
	public sbyte I1;
	public byte U1;
	public short I2;
	public ushort U2;
	public int I4;
	public long I8;
	public float R4;
	public double R8;
	public string O;
	public Fields_Point Point;
	public Fields_Tagged Tagged;
	public volatile int Vol;
}

class Fields_Box<T> {
	public T Value;
}

class Fields_GenStatic<T> {
	public static int N;
	public static T Value;
}

// An explicit static constructor, so the class is not beforefieldinit and the
// runtime has to run the constructor at the first access. Each of the two tests
// gets a class of its own: a class the other test already touched is
// initialized, and answers the test for free.
class Fields_LateRead {
	public static int Value;

	static Fields_LateRead () { Value = 6; }
}

class Fields_LateStore {
	public static int Value;
	public static int Marker;

	static Fields_LateStore ()
	{
		Value = 6;
		Marker = 3;
	}
}

class Fields_Const {
	public static int Marker = 1;
	public static readonly int I4 = 5;
	public static readonly long I8 = 0x100000007L;
	public static readonly double R8 = 2.5;
	public static readonly string O = "abcde";
	public static readonly Fields_Point P = new Fields_Point (3, 4);
}

class Fields_Remote : MarshalByRefObject {
	public int N;
	public Fields_Point Point;
}

[StructLayout (LayoutKind.Explicit)]
struct Fields_Overlap {
	[FieldOffset (0)] public int Word;
	[FieldOffset (0)] public byte Low;
	[FieldOffset (4)] public int High;
}

[StructLayout (LayoutKind.Sequential, Pack = 1)]
struct Fields_Packed {
	public byte Tag;
	public long Value;
}

[NoOpt]
public class Fields {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long IdL (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static float IdF (float x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double IdD (double x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static string IdS (string x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Bump (ref int x) { x += 7; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Fields_Point MakePoint (int x, int y)
	{
		Fields_Point p = new Fields_Point ();

		p.X = x;
		p.Y = y;
		return p;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Fields_Cell NewCell () { return new Fields_Cell (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Fields_Cell NullCell () { return null; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Fields_Cell MakeCell ()
	{
		Fields_Cell c = new Fields_Cell ();

		c.I1 = -1;
		c.U1 = 255;
		c.I2 = -2;
		c.U2 = 65535;
		c.I4 = 42;
		c.I8 = 0x100000000L + 42;
		c.R4 = 1.5f;
		c.R8 = 2.5;
		c.O = "hello";
		c.Point = MakePoint (3, 4);
		c.Tagged.Name = "tag";
		c.Tagged.N = 9;
		return c;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Fields_Mixed MakeMixed ()
	{
		Fields_Mixed m = new Fields_Mixed ();

		m.I1 = -1;
		m.U1 = 255;
		m.I2 = -2;
		m.U2 = 65535;
		m.I4 = 9;
		m.I8 = 0x100000000L + 9;
		m.R4 = 1.5f;
		m.R8 = 2.5;
		m.O = "abcd";
		return m;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Fields_Outer MakeOuter ()
	{
		Fields_Outer o = new Fields_Outer ();

		o.Head = 1;
		o.Inner.X = 3;
		o.Inner.Y = 0x100000000L + 3;
		return o;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Fields_Remote NewRemote () { return new Fields_Remote (); }

	//
	// ldfld, on a reference type.
	//

	public static int test_1_ldfld_narrow ()
	{
		Fields_Cell c = MakeCell ();
		return (c.I1 == -1 && c.U1 == 255 && c.I2 == -2 && c.U2 == 65535) ? 1 : 0;
	}

	public static int test_42_ldfld_i4 ()
	{
		Fields_Cell c = MakeCell ();
		return c.I4;
	}

	public static int test_1_ldfld_i8 ()
	{
		Fields_Cell c = MakeCell ();
		return c.I8 == 0x100000000L + 42 ? 1 : 0;
	}

	public static int test_1_ldfld_r4_r8 ()
	{
		Fields_Cell c = MakeCell ();
		return (c.R4 == 1.5f && c.R8 == 2.5) ? 1 : 0;
	}

	public static int test_5_ldfld_o ()
	{
		Fields_Cell c = MakeCell ();
		return c.O.Length;
	}

	public static int test_7_ldfld_vt ()
	{
		Fields_Cell c = MakeCell ();
		Fields_Point p = c.Point;

		return p.X + p.Y;
	}

	//
	// stfld, on a reference type.
	//

	public static int test_1_stfld_narrow ()
	{
		Fields_Cell c = NewCell ();

		c.I1 = (sbyte) Id (-3);
		c.U1 = (byte) Id (200);
		c.I2 = (short) Id (-300);
		c.U2 = (ushort) Id (40000);
		return (c.I1 == -3 && c.U1 == 200 && c.I2 == -300 && c.U2 == 40000) ? 1 : 0;
	}

	public static int test_7_stfld_i4 ()
	{
		Fields_Cell c = NewCell ();

		c.I4 = Id (7);
		return c.I4;
	}

	public static int test_1_stfld_i8 ()
	{
		Fields_Cell c = NewCell ();

		c.I8 = IdL (-0x100000000L);
		return c.I8 == -0x100000000L ? 1 : 0;
	}

	// The r4 field narrows the value, so 0.1 must not come back as 0.1.
	public static int test_1_stfld_r4_r8 ()
	{
		Fields_Cell c = NewCell ();

		c.R4 = (float) IdD (0.1);
		c.R8 = IdD (0.25);
		return ((double) c.R4 != 0.1 && c.R8 == 0.25) ? 1 : 0;
	}

	public static int test_4_stfld_o ()
	{
		Fields_Cell c = NewCell ();

		c.O = IdS ("abcd");
		return c.O.Length;
	}

	public static int test_9_stfld_vt_noref ()
	{
		Fields_Cell c = NewCell ();
		c.Point = MakePoint (Id (4), Id (5));

		Fields_Point p = c.Point;
		return p.X + p.Y;
	}

	// A value type that holds a reference goes through the write barrier instead
	// of a plain copy.
	public static int test_6_stfld_vt_with_reference ()
	{
		Fields_Cell c = NewCell ();
		Fields_Tagged t = new Fields_Tagged ();

		t.Name = IdS ("abc");
		t.N = Id (3);
		c.Tagged = t;

		Fields_Tagged back = c.Tagged;
		return back.Name.Length + back.N;
	}

	//
	// ldfld off a value type that sits on the stack.
	//

	public static int test_1_ldfld_vt_narrow ()
	{
		return (MakeMixed ().I1 == -1 && MakeMixed ().U1 == 255 &&
		        MakeMixed ().I2 == -2 && MakeMixed ().U2 == 65535) ? 1 : 0;
	}

	public static int test_9_ldfld_vt_i4 ()
	{
		return MakeMixed ().I4;
	}

	public static int test_1_ldfld_vt_i8 ()
	{
		return MakeMixed ().I8 == 0x100000000L + 9 ? 1 : 0;
	}

	public static int test_1_ldfld_vt_r4_r8 ()
	{
		return (MakeMixed ().R4 == 1.5f && MakeMixed ().R8 == 2.5) ? 1 : 0;
	}

	public static int test_4_ldfld_vt_o ()
	{
		return MakeMixed ().O.Length;
	}

	public static int test_1_ldfld_vt_of_vt ()
	{
		return (MakeOuter ().Inner.X == 3 &&
		        MakeOuter ().Inner.Y == 0x100000000L + 3) ? 1 : 0;
	}

	//
	// Field addresses.
	//

	public static int test_9_ldflda_object ()
	{
		Fields_Cell c = NewCell ();

		c.I4 = Id (2);
		Bump (ref c.I4);
		return c.I4;
	}

	// The receiver is a managed pointer rather than an object, so the transform
	// emits no null check.
	public static int test_9_ldflda_unsafe ()
	{
		Fields_Point p = MakePoint (Id (2), 0);

		Bump (ref p.X);
		return p.X;
	}

	public static int test_9_field_through_this ()
	{
		Fields_Counter c = new Fields_Counter ();

		c.A = Id (1);
		c.B = IdL (2);
		c.Add (Id (3));
		return c.Total ();
	}

	// One bit per site, so a site that does not throw names itself: it returns 0,
	// or it leaves its bit clear.
	public static int test_15_null_checks ()
	{
		Fields_Cell c = NullCell ();
		int hits = 0;

		try { if (c.I4 == 0) return 0; } catch (NullReferenceException) { hits |= 1; }
		try { Fields_Point p = c.Point; if (p.X == 0) return 0; } catch (NullReferenceException) { hits |= 2; }
		try { c.I4 = Id (5); return 0; } catch (NullReferenceException) { hits |= 4; }
		try { Bump (ref c.I4); return 0; } catch (NullReferenceException) { hits |= 8; }
		return hits;
	}

	//
	// Static fields.
	//

	static sbyte sI1;
	static byte sU1;
	static short sI2;
	static ushort sU2;
	static int sI4;
	static long sI8;
	static float sR4;
	static double sR8;
	static string sO;
	static Fields_Point sPoint;
	static Fields_Tagged sTagged;
	static int sBump;

	public static int test_1_static_narrow ()
	{
		sI1 = (sbyte) Id (-1);
		sU1 = (byte) Id (255);
		sI2 = (short) Id (-2);
		sU2 = (ushort) Id (65535);
		return (sI1 == -1 && sU1 == 255 && sI2 == -2 && sU2 == 65535) ? 1 : 0;
	}

	public static int test_11_static_i4_i8_r4_r8 ()
	{
		sI4 = Id (11);
		sI8 = IdL (0x100000000L + 5);
		sR4 = IdF (0.5f);
		sR8 = IdD (0.25);
		if (sI8 != 0x100000000L + 5 || sR4 != 0.5f || sR8 != 0.25)
			return 0;
		return sI4;
	}

	public static int test_3_static_o ()
	{
		sO = IdS ("abc");
		return sO.Length;
	}

	public static int test_7_static_vt ()
	{
		sPoint = MakePoint (Id (3), Id (4));

		Fields_Point p = sPoint;
		return p.X + p.Y;
	}

	// A struct that carries a reference goes into a static field and into a
	// thread-static one. The reference must survive a collection through both.
	public static int test_6_static_vt_with_reference ()
	{
		Fields_Tagged t = new Fields_Tagged ();

		t.Name = IdS ("abc");
		t.N = Id (3);
		sTagged = t;
		tsTagged = t;
		GC.Collect ();

		Fields_Tagged fromStatic = sTagged;
		Fields_Tagged fromThread = tsTagged;
		return fromStatic.Name.Length + fromThread.N;
	}

	// sBump starts at a value of its own, so an address that names the wrong
	// field gives 7 instead of 9.
	public static int test_9_ldsflda ()
	{
		sBump = Id (2);
		Bump (ref sBump);
		return sBump;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadReadonlyScalars ()
	{
		return (Fields_Const.I4 == 5 && Fields_Const.I8 == 0x100000007L &&
		        Fields_Const.R8 == 2.5) ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadReadonlyWide ()
	{
		Fields_Point p = Fields_Const.P;

		return (Fields_Const.O.Length == 5 && p.X == 3 && p.Y == 4) ? 1 : 0;
	}

	// Reading Marker initializes the class. The transform then sees an initialized
	// vtable and folds the reads in ReadReadonlyScalars into constants.
	public static int test_1_static_readonly_folds ()
	{
		return Fields_Const.Marker == 1 ? ReadReadonlyScalars () : 0;
	}

	// A reference and a value type have no constant to fold to, so
	// ReadReadonlyWide loads them. It is the negative control for the test above.
	public static int test_1_static_readonly_wide ()
	{
		return Fields_Const.Marker == 1 ? ReadReadonlyWide () : 0;
	}

	public static int test_6_cctor_runs_before_first_read ()
	{
		return Fields_LateRead.Value;
	}

	// The constructor sets Value to 6 and Marker to 3, so the sum says whether it
	// ran, and whether it ran before the store rather than after it.
	public static int test_13_cctor_runs_before_first_store ()
	{
		Fields_LateStore.Value = Id (10);
		return Fields_LateStore.Marker + Fields_LateStore.Value;
	}

	// Each instantiation gets storage of its own, so the two N fields hold
	// different values.
	public static int test_6_generic_static_fields ()
	{
		Fields_GenStatic<int>.N = Id (2);
		Fields_GenStatic<string>.N = Id (3);
		Fields_GenStatic<string>.Value = IdS ("x");
		return Fields_GenStatic<int>.N + Fields_GenStatic<string>.N +
		       Fields_GenStatic<string>.Value.Length;
	}

	// The transform answers this one out of the target's byte order instead of
	// reading the field.
	public static int test_1_bitconverter_is_little_endian ()
	{
		return BitConverter.IsLittleEndian ? 1 : 0;
	}

	//
	// Thread statics, whose address comes out of the thread rather than out of
	// the vtable.
	//

	[ThreadStatic] static sbyte tsI1;
	[ThreadStatic] static byte tsU1;
	[ThreadStatic] static short tsI2;
	[ThreadStatic] static ushort tsU2;
	[ThreadStatic] static int tsI4;
	[ThreadStatic] static long tsI8;
	[ThreadStatic] static float tsR4;
	[ThreadStatic] static double tsR8;
	[ThreadStatic] static string tsO;
	[ThreadStatic] static Fields_Point tsPoint;
	[ThreadStatic] static Fields_Tagged tsTagged;

	// Nothing writes these three. They are the storage test_1_threadstatic_starts_at_zero
	// reads, and a field another test wrote would answer it for free.
	[ThreadStatic] static int tsFreshI4;
	[ThreadStatic] static long tsFreshI8;
	[ThreadStatic] static string tsFreshO;

	public static int test_1_threadstatic_narrow ()
	{
		tsI1 = (sbyte) Id (-1);
		tsU1 = (byte) Id (255);
		tsI2 = (short) Id (-2);
		tsU2 = (ushort) Id (65535);
		return (tsI1 == -1 && tsU1 == 255 && tsI2 == -2 && tsU2 == 65535) ? 1 : 0;
	}

	public static int test_9_threadstatic_i4_i8_r4_r8 ()
	{
		tsI4 = Id (9);
		tsI8 = IdL (0x100000000L + 5);
		tsR4 = IdF (0.5f);
		tsR8 = IdD (0.25);
		if (tsI8 != 0x100000000L + 5 || tsR4 != 0.5f || tsR8 != 0.25)
			return 0;
		return tsI4;
	}

	public static int test_4_threadstatic_o ()
	{
		tsO = IdS ("abcd");
		return tsO.Length;
	}

	// A thread static too wide for a scalar opcode takes the special-static path
	// instead of the packed thread-static offset.
	public static int test_7_threadstatic_vt ()
	{
		tsPoint = MakePoint (Id (3), Id (4));

		Fields_Point p = tsPoint;
		return p.X + p.Y;
	}

	public static int test_5_threadstatic_address ()
	{
		tsI4 = Id (-2);
		Bump (ref tsI4);
		return tsI4;
	}

	// This reads a thread static before anything writes it, so the thread's
	// storage block has to exist already.
	public static int test_1_threadstatic_starts_at_zero ()
	{
		return (tsFreshI4 == 0 && tsFreshI8 == 0 && tsFreshO == null) ? 1 : 0;
	}

	//
	// A field on a MarshalByRefObject, which the transform reaches through a
	// remoting wrapper rather than with an offset.
	//

	public static int test_16_marshalbyref_fields ()
	{
		Fields_Remote r = NewRemote ();

		r.N = Id (9);
		r.Point = MakePoint (Id (3), Id (4));

		Fields_Point p = r.Point;
		return r.N + p.X + p.Y;
	}

	// CS0197 warns that the address of a marshal-by-reference field can fault.
	// The test is for the ldflda wrapper this shape goes through.
#pragma warning disable 197
	public static int test_9_marshalbyref_ldflda ()
	{
		Fields_Remote r = NewRemote ();

		r.N = Id (2);
		Bump (ref r.N);
		return r.N;
	}
#pragma warning restore 197

	//
	// Field shapes that reach an ordinary offset by a different route.
	//

	// A volatile access puts a memory barrier around the load and the store.
	public static int test_1_volatile_field ()
	{
		Fields_Cell c = NewCell ();

		c.Vol = Id (11);
		return c.Vol == 11 ? 1 : 0;
	}

	public static int test_3_generic_class_fields ()
	{
		Fields_Box<int> bi = new Fields_Box<int> ();
		Fields_Box<string> bs = new Fields_Box<string> ();

		bi.Value = Id (2);
		bs.Value = IdS ("x");
		return bi.Value + bs.Value.Length;
	}

	public static int test_1_explicit_layout_overlaps ()
	{
		Fields_Overlap o = new Fields_Overlap ();

		o.Word = Id (0x11223344);
		o.High = Id (7);
		return (o.Low == 0x44 && o.Word == 0x11223344 && o.High == 7) ? 1 : 0;
	}

	public static int test_1_packed_layout_i8 ()
	{
		Fields_Packed p = new Fields_Packed ();

		p.Tag = (byte) Id (3);
		p.Value = IdL (0x0102030405060708L);
		return (p.Tag == 3 && p.Value == 0x0102030405060708L) ? 1 : 0;
	}
}
