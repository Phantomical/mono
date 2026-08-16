// Converting one argument or return value between the runtime's representation
// and the interpreter's stack one.
//
// stackval_from_data () and stackval_to_data () have a case per MonoType, and
// there are two copies of them. mono_interp_entry () calls the copy in
// interp-internals.hpp, so reflection and a delegate handed to native code go
// there. init_arglist () and do_icall () call the copy in interp.c, so a vararg
// call site and the return of a native call go there instead. The tests below
// are grouped by which of those four routes they drive.
//
// runtimeentry.cs already drives the primitive cases through reflection. This
// file adds the shapes it does not reach: enums, pointers, native unsigned,
// arrays, generic instances and a TypedReference.

using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

public enum SignaturesEnumI1 : sbyte { Low = -3, High = 100 }
public enum SignaturesEnumU1 : byte { Low = 3, High = 200 }
public enum SignaturesEnumI2 : short { Low = -300, High = 30000 }
public enum SignaturesEnumU2 : ushort { Low = 300, High = 60000 }
public enum SignaturesEnumI4 : int { Low = -70000, High = 70000 }
public enum SignaturesEnumU4 : uint { Low = 7, High = 4000000000 }
public enum SignaturesEnumI8 : long { Low = -0x100000001L, High = 0x100000001L }
public enum SignaturesEnumU8 : ulong { Low = 8, High = 0x8000000000000001UL }

public struct SignaturesS1 { public byte A; }
public struct SignaturesS4 { public int A; }
public struct SignaturesS16 { public long A; public long B; }

public struct SignaturesS8 {
	public long A;
	[MethodImpl (MethodImplOptions.NoInlining)]
	public long Bump () { return A + 1; }
}

public struct SignaturesRefs {
	public string Name;
	public object Tag;
	public int Count;
}

public struct SignaturesBox<T> { public T Value; }

public class SignaturesCell<T> { public T Value; }

public interface ISignaturesThing { int Bump (int x); }

public class SignaturesThing : ISignaturesThing {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public int Bump (int x) { return x + 1; }
}

public delegate int SignaturesAdder (int a, int b);

public delegate SignaturesS8 SignaturesS8Func (SignaturesS8 v);
public delegate SignaturesS4 SignaturesS4Func (int n);
public delegate SignaturesEnumI8 SignaturesEnumFunc (SignaturesEnumU1 a, SignaturesEnumI2 b,
                                                     SignaturesEnumU4 c);
public delegate string SignaturesStringFunc (string s);
public unsafe delegate int SignaturesPtrFunc (int *p);
public unsafe delegate int* SignaturesPtrRetFunc (int *p);
public delegate void SignaturesByRefFunc (ref int x);

public class SignaturesTarget {

	public static SignaturesEnumI1 TakeEnumI1 (SignaturesEnumI1 e) { return (SignaturesEnumI1) ((sbyte) e + 1); }
	public static SignaturesEnumU1 TakeEnumU1 (SignaturesEnumU1 e) { return (SignaturesEnumU1) ((byte) e + 1); }
	public static SignaturesEnumI2 TakeEnumI2 (SignaturesEnumI2 e) { return (SignaturesEnumI2) ((short) e + 1); }
	public static SignaturesEnumU2 TakeEnumU2 (SignaturesEnumU2 e) { return (SignaturesEnumU2) ((ushort) e + 1); }
	public static SignaturesEnumI4 TakeEnumI4 (SignaturesEnumI4 e) { return (SignaturesEnumI4) ((int) e + 1); }
	public static SignaturesEnumU4 TakeEnumU4 (SignaturesEnumU4 e) { return (SignaturesEnumU4) ((uint) e + 1); }
	public static SignaturesEnumI8 TakeEnumI8 (SignaturesEnumI8 e) { return (SignaturesEnumI8) ((long) e + 1); }
	public static SignaturesEnumU8 TakeEnumU8 (SignaturesEnumU8 e) { return (SignaturesEnumU8) ((ulong) e + 1); }

	public static int? TakeNullable (int? x) { return x.HasValue ? (int?) (x.Value + 1) : null; }

	public static UIntPtr TakeUIntPtr (UIntPtr x) { return (UIntPtr) ((ulong) x + 1); }

	public static int TakeArray (int [] a) { return a [0] + a [1]; }
	public static int [] MakeArray (int n) { return new int [] { n, n + 1 }; }

	public static int TakeMatrix (int [,] m) { return m [1, 1] + m [0, 1]; }
	public static int [,] MakeMatrix (int n)
	{
		int [,] m = new int [2, 2];
		m [0, 1] = n;
		m [1, 1] = n + 1;
		return m;
	}

	public static SignaturesBox<int> TakeBoxInt (SignaturesBox<int> b) { b.Value++; return b; }
	public static SignaturesCell<int> TakeCellInt (SignaturesCell<int> c) { c.Value++; return c; }

	public static int TakeInterface (ISignaturesThing t) { return t.Bump (10); }
	public static ISignaturesThing MakeInterface () { return new SignaturesThing (); }

	public static int AddTwo (int a, int b) { return a + b; }

	public static void BumpByRef (ref int x) { x += 5; }

	public static SignaturesS1 TakeS1 (SignaturesS1 v) { v.A++; return v; }
	public static SignaturesS8 TakeS8 (SignaturesS8 v) { v.A++; return v; }

	public static SignaturesRefs TakeRefs (SignaturesRefs r)
	{
		r.Name += "!";
		r.Tag = r.Name;
		r.Count++;
		return r;
	}

	public static string TakeObject (object o) { return o.ToString (); }
	public static object MakeObject (int n) { return n; }

	public static T Echo<T> (T x) { return x; }

	public unsafe static int TakePointer (int *p) { return *p + 1; }
	public unsafe static int* BumpPointer (int *p) { return p + 1; }

	public static long TakeMixed (SignaturesS4 a, SignaturesEnumI8 b, decimal c, string d,
	                              SignaturesS16 e)
	{
		return a.A + (long) b + (long) c + d.Length + e.A;
	}
}

public class Signatures {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long IdLong (long x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object Invoke (string name, params object [] args)
	{
		return typeof (SignaturesTarget).GetMethod (name).Invoke (null, args);
	}

	// Reflection: a compiled runtime-invoke wrapper calls the interpreted method.

	// An enum arrives as its underlying type, so each width takes a different
	// case after the one recursion.

	public static int test_1_invoke_enum_narrow ()
	{
		return (SignaturesEnumI1) Invoke ("TakeEnumI1", SignaturesEnumI1.High) == (SignaturesEnumI1) 101 &&
		       (SignaturesEnumU1) Invoke ("TakeEnumU1", SignaturesEnumU1.High) == (SignaturesEnumU1) 201 &&
		       (SignaturesEnumI2) Invoke ("TakeEnumI2", SignaturesEnumI2.High) == (SignaturesEnumI2) 30001 &&
		       (SignaturesEnumU2) Invoke ("TakeEnumU2", SignaturesEnumU2.High) == (SignaturesEnumU2) 60001 ? 1 : 0;
	}

	public static int test_1_invoke_enum_wide ()
	{
		return (SignaturesEnumI4) Invoke ("TakeEnumI4", SignaturesEnumI4.High) == (SignaturesEnumI4) 70001 &&
		       (SignaturesEnumU4) Invoke ("TakeEnumU4", SignaturesEnumU4.High) == (SignaturesEnumU4) 4000000001 &&
		       (SignaturesEnumI8) Invoke ("TakeEnumI8", SignaturesEnumI8.High) == (SignaturesEnumI8) 0x100000002L &&
		       (SignaturesEnumU8) Invoke ("TakeEnumU8", SignaturesEnumU8.High) == (SignaturesEnumU8) 0x8000000000000002UL ? 1 : 0;
	}

	// A negative value has to survive the narrow cases, which sign extend.
	public static int test_1_invoke_enum_negative ()
	{
		return (SignaturesEnumI1) Invoke ("TakeEnumI1", SignaturesEnumI1.Low) == (SignaturesEnumI1) (-2) &&
		       (SignaturesEnumI2) Invoke ("TakeEnumI2", SignaturesEnumI2.Low) == (SignaturesEnumI2) (-299) &&
		       (SignaturesEnumI4) Invoke ("TakeEnumI4", SignaturesEnumI4.Low) == (SignaturesEnumI4) (-69999) ? 1 : 0;
	}

	public static int test_6_invoke_nullable ()
	{
		return (int) Invoke ("TakeNullable", (int?) 5);
	}

	// UIntPtr is MONO_TYPE_U, a different case from the IntPtr one either side.
	public static int test_1_invoke_uintptr ()
	{
		return (ulong) (UIntPtr) Invoke ("TakeUIntPtr", (UIntPtr) 4) == 5UL ? 1 : 0;
	}

	public static int test_3_invoke_array_argument ()
	{
		return (int) Invoke ("TakeArray", new object [] { new int [] { 1, 2 } });
	}

	public static int test_1_invoke_array_return ()
	{
		int [] r = (int []) Invoke ("MakeArray", 4);
		return r.Length == 2 && r [0] == 4 && r [1] == 5 ? 1 : 0;
	}

	// A rank-2 array is MONO_TYPE_ARRAY, which is a separate case from SZARRAY.

	public static int test_9_invoke_multidim_array_argument ()
	{
		int [,] m = new int [2, 2];
		m [0, 1] = 4;
		m [1, 1] = 5;
		return (int) Invoke ("TakeMatrix", new object [] { m });
	}

	public static int test_1_invoke_multidim_array_return ()
	{
		int [,] m = (int [,]) Invoke ("MakeMatrix", 6);
		return m [0, 1] == 6 && m [1, 1] == 7 ? 1 : 0;
	}

	public static int test_1_invoke_generic_struct ()
	{
		SignaturesBox<int> b = new SignaturesBox<int> { Value = 8 };
		return ((SignaturesBox<int>) Invoke ("TakeBoxInt", b)).Value == 9 ? 1 : 0;
	}

	// A generic instance over a class is not a value type, so the conversion
	// recurses to the container class instead of copying.
	public static int test_1_invoke_generic_class ()
	{
		SignaturesCell<int> c = new SignaturesCell<int> { Value = 2 };
		return ((SignaturesCell<int>) Invoke ("TakeCellInt", c)).Value == 3 ? 1 : 0;
	}

	public static int test_11_invoke_interface_argument ()
	{
		return (int) Invoke ("TakeInterface", new SignaturesThing ());
	}

	public static int test_12_invoke_interface_return ()
	{
		ISignaturesThing t = (ISignaturesThing) Invoke ("MakeInterface");
		return t.Bump (11);
	}

	// A byref argument is handed over as the address itself, so the entry takes
	// it without converting anything and the callee writes through.
	public static int test_10_invoke_byref_parameter ()
	{
		object [] args = new object [] { Id (5) };
		typeof (SignaturesTarget).GetMethod ("BumpByRef").Invoke (null, args);
		return (int) args [0];
	}

	// One byte on the stack takes a whole slot, so the size the conversion
	// reports is the rounded one.
	public static int test_1_invoke_struct_1_byte ()
	{
		SignaturesS1 v = new SignaturesS1 { A = 200 };
		return ((SignaturesS1) Invoke ("TakeS1", v)).A == 201 ? 1 : 0;
	}

	public static int test_1_invoke_struct_with_references ()
	{
		SignaturesRefs v = new SignaturesRefs { Name = "n", Tag = null, Count = 1 };
		SignaturesRefs r = (SignaturesRefs) Invoke ("TakeRefs", v);
		return r.Name == "n!" && (string) r.Tag == "n!" && r.Count == 2 ? 1 : 0;
	}

	public static int test_1_invoke_object_argument ()
	{
		return (string) Invoke ("TakeObject", "text") == "text" ? 1 : 0;
	}

	public static int test_13_invoke_object_return ()
	{
		return (int) Invoke ("MakeObject", Id (13));
	}

	public static int test_1_invoke_generic_method_over_struct ()
	{
		MethodInfo m = typeof (SignaturesTarget).GetMethod ("Echo")
			.MakeGenericMethod (typeof (SignaturesS16));
		SignaturesS16 v = new SignaturesS16 { A = IdLong (3), B = IdLong (4) };
		SignaturesS16 r = (SignaturesS16) m.Invoke (null, new object [] { v });
		return r.A == 3 && r.B == 4 ? 1 : 0;
	}

	// Arguments of different sizes in one signature: each moves the write
	// position by the size the conversion reports, so a wrong size in the middle
	// displaces every argument behind it.
	public static int test_1_invoke_mixed_value_types ()
	{
		SignaturesS4 a = new SignaturesS4 { A = 1 };
		SignaturesS16 e = new SignaturesS16 { A = 5, B = 6 };
		long r = (long) Invoke ("TakeMixed", a, SignaturesEnumI8.High, 3m, "ab", e);
		return r == 1 + 0x100000001L + 3 + 2 + 5 ? 1 : 0;
	}

	// The call boxes the receiver of an instance method on a value type, and the
	// unbox happens before the entry. The entry takes a plain address and
	// converts nothing.
	public static int test_8_invoke_struct_instance_method ()
	{
		object boxed = new SignaturesS8 { A = IdLong (7) };
		return (int) (long) typeof (SignaturesS8).GetMethod ("Bump").Invoke (boxed, null);
	}

	// Native code calling back in: the two marshalling wrappers around the round
	// trip are always compiled, so the callback is entered from compiled code.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Delegate RoundTrip (Delegate d)
	{
		return Marshal.GetDelegateForFunctionPointer (
			Marshal.GetFunctionPointerForDelegate (d), d.GetType ());
	}

	static SignaturesEnumI8 Combine (SignaturesEnumU1 a, SignaturesEnumI2 b, SignaturesEnumU4 c)
	{
		return (SignaturesEnumI8) ((long) a + (long) b + (long) c);
	}

	public static int test_1_callback_struct_8_bytes ()
	{
		SignaturesS8Func f = SignaturesTarget.TakeS8;
		SignaturesS8Func back = (SignaturesS8Func) RoundTrip (f);
		return back (new SignaturesS8 { A = IdLong (5) }).A == 6L ? 1 : 0;
	}

	public static int test_1_callback_struct_return_only ()
	{
		SignaturesS4Func f = MakeS4;
		SignaturesS4Func back = (SignaturesS4Func) RoundTrip (f);
		return back (Id (20)).A == 21 ? 1 : 0;
	}

	static SignaturesS4 MakeS4 (int n) { return new SignaturesS4 { A = n + 1 }; }

	public static int test_1_callback_enum_widths ()
	{
		SignaturesEnumFunc f = Combine;
		SignaturesEnumFunc back = (SignaturesEnumFunc) RoundTrip (f);
		SignaturesEnumI8 r = back (SignaturesEnumU1.High, SignaturesEnumI2.Low,
		                           SignaturesEnumU4.High);
		return (long) r == 200 - 300 + 4000000000L ? 1 : 0;
	}

	// A pointer is MONO_TYPE_PTR, and reflection cannot reach that case: this
	// runtime's binder refuses a boxed pointer as an argument.

	public static unsafe int test_43_callback_pointer ()
	{
		SignaturesPtrFunc f = SignaturesTarget.TakePointer;
		SignaturesPtrFunc back = (SignaturesPtrFunc) RoundTrip (f);
		int v = Id (42);
		return back (&v);
	}

	public static unsafe int test_1_callback_pointer_return ()
	{
		SignaturesPtrRetFunc f = SignaturesTarget.BumpPointer;
		SignaturesPtrRetFunc back = (SignaturesPtrRetFunc) RoundTrip (f);
		int *pair = stackalloc int [2];
		pair [0] = Id (1);
		pair [1] = Id (2);
		return *back (pair) == 2 ? 1 : 0;
	}

	public static int test_15_callback_byref_parameter ()
	{
		SignaturesByRefFunc f = SignaturesTarget.BumpByRef;
		SignaturesByRefFunc back = (SignaturesByRefFunc) RoundTrip (f);
		int x = Id (5);
		back (ref x);
		back (ref x);
		return x;
	}

	public static int test_1_callback_string ()
	{
		SignaturesStringFunc f = Shout;
		SignaturesStringFunc back = (SignaturesStringFunc) RoundTrip (f);
		return back ("ok") == "ok!" ? 1 : 0;
	}

	static string Shout (string s) { return s + "!"; }

	// A vararg call site: the interp.c copy of the conversion lays the variable
	// arguments out for an ArgIterator, with the types the caller wrote rather
	// than the ones the callee declares. So a vararg site is where the width
	// coverage for that copy goes.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long ArgSumIntegers (__arglist)
	{
		ArgIterator it = new ArgIterator (__arglist);
		long total = __refvalue (it.GetNextArg (), sbyte);
		total += __refvalue (it.GetNextArg (), byte);
		total += __refvalue (it.GetNextArg (), short);
		total += __refvalue (it.GetNextArg (), ushort);
		total += __refvalue (it.GetNextArg (), int);
		total += __refvalue (it.GetNextArg (), uint);
		total += __refvalue (it.GetNextArg (), long);
		total += (long) __refvalue (it.GetNextArg (), ulong);
		total += __refvalue (it.GetNextArg (), char);
		total += __refvalue (it.GetNextArg (), bool) ? 1 : 0;
		return total;
	}

	public static int test_46_arglist_integer_widths ()
	{
		return (int) ArgSumIntegers (__arglist ((sbyte) 1, (byte) 2, (short) 3, (ushort) 4,
		                                        Id (5), 6u, 7L, 8ul, (char) 9, true));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe double ArgSumScalars (__arglist)
	{
		ArgIterator it = new ArgIterator (__arglist);
		double total = __refvalue (it.GetNextArg (), float);
		total += __refvalue (it.GetNextArg (), double);
		total += (long) __refvalue (it.GetNextArg (), IntPtr);
		total += (long) (ulong) __refvalue (it.GetNextArg (), UIntPtr);
		total += *__refvalue (it.GetNextArg (), int*);
		return total;
	}

	public static unsafe int test_1_arglist_floats_and_pointers ()
	{
		int local = Id (5);
		double r = ArgSumScalars (__arglist (1.5f, 2.25, (IntPtr) 3, (UIntPtr) 4, &local));
		return r == 15.75 ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string ArgJoinReferences (__arglist)
	{
		ArgIterator it = new ArgIterator (__arglist);
		string s = __refvalue (it.GetNextArg (), string);
		object o = __refvalue (it.GetNextArg (), object);
		int [] a = __refvalue (it.GetNextArg (), int []);
		int [,] m = __refvalue (it.GetNextArg (), int [,]);
		SignaturesAdder d = __refvalue (it.GetNextArg (), SignaturesAdder);
		return s + o + a [0] + m [1, 1] + d (1, 1);
	}

	public static int test_1_arglist_references ()
	{
		int [,] m = new int [2, 2];
		m [1, 1] = 8;
		string r = ArgJoinReferences (__arglist ("a", (object) "b", new int [] { 7 }, m,
		                                         new SignaturesAdder (SignaturesTarget.AddTwo)));
		return r == "ab782" ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long ArgSumStructs (__arglist)
	{
		ArgIterator it = new ArgIterator (__arglist);
		long total = (long) __refvalue (it.GetNextArg (), SignaturesEnumI8);
		SignaturesS16 s = __refvalue (it.GetNextArg (), SignaturesS16);
		total += s.A + s.B;
		total += __refvalue (it.GetNextArg (), SignaturesS1).A;
		total += __refvalue (it.GetNextArg (), SignaturesRefs).Count;
		return total;
	}

	public static int test_1_arglist_value_types ()
	{
		long r = ArgSumStructs (__arglist (SignaturesEnumI8.High,
		                                   new SignaturesS16 { A = 1, B = 2 },
		                                   new SignaturesS1 { A = 4 },
		                                   new SignaturesRefs { Name = "n", Count = 8 }));
		return r == 0x100000001L + 15 ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long ArgSumGenerics (__arglist)
	{
		ArgIterator it = new ArgIterator (__arglist);
		long total = __refvalue (it.GetNextArg (), SignaturesBox<int>).Value;
		total += __refvalue (it.GetNextArg (), SignaturesCell<int>).Value;
		total += (long) __refvalue (it.GetNextArg (), decimal);
		total += __refvalue (it.GetNextArg (), DateTime).Ticks;
		total += __refvalue (it.GetNextArg (), Nullable<int>).Value;
		return total;
	}

	public static int test_1_arglist_generic_instances ()
	{
		long r = ArgSumGenerics (__arglist (new SignaturesBox<int> { Value = 4 },
		                                    new SignaturesCell<int> { Value = 5 },
		                                    6m, new DateTime (7L), (int?) 8));
		return r == 30 ? 1 : 0;
	}

	// A TypedReference cannot be boxed, so a vararg site is the only way a test
	// hands one to the conversion. Reading the target type back proves all three
	// words crossed, not just the address in them.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ArgBumpTypedRef (__arglist)
	{
		ArgIterator it = new ArgIterator (__arglist);
		TypedReference tr = __refvalue (it.GetNextArg (), TypedReference);
		if (TypedReference.GetTargetType (tr) != typeof (int))
			return -1;
		int seen = __refvalue (tr, int);
		__refvalue (tr, int) = seen + 1;
		return seen;
	}

	public static int test_1_arglist_typed_reference ()
	{
		int x = Id (5);
		int seen = ArgBumpTypedRef (__arglist (__makeref (x)));
		return seen == 5 && x == 6 ? 1 : 0;
	}

	// A native call that hands back a raw address: the return is converted as a
	// pointer rather than as a native integer, by the interp.c copy again.
	[DllImport ("__Internal", EntryPoint = "interp_test_strchr")]
	static unsafe extern byte* NativeStrchr (byte *s, int c);

	public static unsafe int test_1_pinvoke_pointer_return ()
	{
		byte [] text = { (byte) 'h', (byte) 'e', (byte) 'l', (byte) 'l', (byte) 'o', 0 };
		fixed (byte *s = text) {
			byte *p = NativeStrchr (s, Id ('l'));
			return p != null && p [0] == (byte) 'l' && p [1] == (byte) 'l' ? 1 : 0;
		}
	}

	// The frame-data allocator, which the vararg opcode above shares with
	// localloc. It keeps the fragment behind the current one for reuse, and a
	// later request the spare cannot hold releases it -- the only way a fragment
	// goes back before the thread ends. The first request fills the initial
	// fragment, the second makes the spare, and the larger third one does not
	// fit the spare.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe int Touch (byte *p, int n)
	{
		for (int i = 0; i < n; i += 512)
			p [i] = (byte) i;
		return p [0] + n;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe int Alloca (int n)
	{
		byte *p = stackalloc byte [n];
		return Touch (p, n);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe int AllocaNested (int outer, int inner)
	{
		byte *p = stackalloc byte [outer];
		return Touch (p, outer) + Alloca (inner);
	}

	public static int test_1_frame_data_fragment_recycled ()
	{
		int a = AllocaNested (Id (8000), Id (3000));
		int b = AllocaNested (Id (8000), Id (5000));
		return a == 11000 && b == 13000 ? 1 : 0;
	}
}
