// Value type copies that the interpreter makes through an address, and the
// pinvoke return that takes the address of a marshalled value type.
//
// A method named test_<n>_<what> is a test. It passes when it returns <n>.
// A NoInlining helper carries each operand to the opcode, so the transform
// cannot fold the answer and leave the opcode untested.
//
// valuetypes.cs holds the ldobj and stobj widths, and the value type copies
// between locals. What is here is what that file does not reach.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

public class ObjCopy {

	[MethodImpl (MethodImplOptions.NoInlining)] static int ObjId (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static string ObjIdStr (string x) { return x; }

	// ldobj !!T then stobj !!T. The transform picks the load and the store from
	// what T instantiates to.
	[MethodImpl (MethodImplOptions.NoInlining)] static void ObjMove<T> (ref T d, ref T s) { d = s; }
	[MethodImpl (MethodImplOptions.NoInlining)] static void ObjStore<T> (ref T d, T v) { d = v; }

	struct ObjRefs { public string Name; public int N; }
	struct ObjEmpty { }

	class ObjHolder { public ObjRefs V; }

	// ------------------------------------------------------------ stobj i2

	// The element on each side of the destination says how wide the store was.
	public static int test_7_stobj_short ()
	{
		short [] a = { 11, 0, 22 };
		int r = 0;

		ObjStore (ref a [ObjId (1)], (short) ObjId (-300));
		if (a [1] == -300) r |= 1;
		if (a [0] == 11) r |= 2;
		if (a [2] == 22) r |= 4;
		return r;
	}

	// ----------------------------------------------- stobj vt into the heap

	// A value type that holds a reference, copied into a field and into an
	// array element. Both destinations are on the heap, so the copy takes the
	// write barrier and the collector must find the string through its new home.
	public static int test_3_stobj_vt_into_the_heap ()
	{
		int r = 0;
		ObjRefs v = default (ObjRefs);

		ObjHolder h = new ObjHolder ();
		v.Name = ObjIdStr ("field");
		v.N = ObjId (5);
		ObjMove (ref h.V, ref v);

		ObjRefs [] a = new ObjRefs [3];
		v.Name = ObjIdStr ("element");
		v.N = ObjId (6);
		ObjMove (ref a [ObjId (2)], ref v);

		GC.Collect ();
		if (h.V.Name == "field" && h.V.N == 5) r |= 1;
		if (a [2].Name == "element" && a [2].N == 6) r |= 2;
		return r;
	}

	// An empty value type has nothing to compare. The guard local is what the
	// test reads back, so a copy that disturbs the frame gives the wrong answer.
	public static int test_1_move_empty_struct ()
	{
		ObjEmpty a = default (ObjEmpty), b = default (ObjEmpty);
		int guard = ObjId (1);

		ObjMove (ref b, ref a);
		return guard;
	}

	// --------------------------------------------- value type across pinvoke

	// native: struct { int quot; int rem; } div (int numer, int denom);
	[StructLayout (LayoutKind.Sequential)]
	struct ObjDiv { public int Quot, Rem; }

	// The same native struct, described so that the second field needs
	// marshalling. That takes the wrapper down the path that loads the address
	// of the returned value type.
	[StructLayout (LayoutKind.Sequential)]
	struct ObjDivBool {
		public int Quot;
		[MarshalAs (UnmanagedType.Bool)] public bool RemSet;
	}

	[DllImport ("libc.so.6", EntryPoint = "div")]
	static extern ObjDiv ObjNativeDiv (int n, int d);

	[DllImport ("libc.so.6", EntryPoint = "div")]
	static extern ObjDivBool ObjNativeDivBool (int n, int d);

	public static int test_3_pinvoke_blittable_struct_return ()
	{
		int r = 0;

		ObjDiv a = ObjNativeDiv (ObjId (7), ObjId (3));
		if (a.Quot == 2 && a.Rem == 1) r |= 1;
		ObjDiv b = ObjNativeDiv (ObjId (9), ObjId (3));
		if (b.Quot == 3 && b.Rem == 0) r |= 2;
		return r;
	}

	public static int test_3_pinvoke_marshalled_struct_return ()
	{
		int r = 0;

		ObjDivBool a = ObjNativeDivBool (ObjId (7), ObjId (3));
		if (a.Quot == 2 && a.RemSet) r |= 1;
		ObjDivBool b = ObjNativeDivBool (ObjId (9), ObjId (3));
		if (b.Quot == 3 && !b.RemSet) r |= 2;
		return r;
	}
}
