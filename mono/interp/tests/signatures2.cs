// The odd types in stackval_from_data () and stackval_to_data (), by the routes
// signatures.cs does not take.
//
// There are two copies of the pair. The one in interp.c serves the native call,
// and is the only one that ever sees the pinvoke flag set:
// interp_frame_arg_to_data () lays the arguments out for the native frame and
// interp_data_to_frame_arg () takes the return value back. The widths below are
// therefore declared on a libc function rather than driven through reflection.
//
// The copy in interp-internals.hpp serves the opcodes and the entry from the
// runtime. Reflection already drives its ordinary cases, and a context static is
// the one field access that keeps the generic load and store. The field's type
// picks the case there, so a pointer field reaches the case nothing else does.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

// div_t, as div () fills it in, behind a generic parameter.
public struct Sig2DivOf<T> {
	public T Quot;
	public T Rem;
}

public struct Sig2Word {
	public int Value;
}

public struct Sig2Box<T> {
	public T Value;
}

public class Sig2Statics {
	[ContextStatic] public unsafe static int *Pointer;
}

public class Signatures2 {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double IdD (double x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static string IdS (string s) { return s; }

	// A native call's return value, converted with the pinvoke flag on. libc
	// returns an int whatever the declaration says. A result whose low bytes
	// differ from the whole int separates a read of the declared width from a
	// read of four.

	[DllImport ("libc.so.6", EntryPoint = "abs")] static extern sbyte AbsAsI1 (int v);
	[DllImport ("libc.so.6", EntryPoint = "abs")] static extern short AbsAsI2 (int v);
	[DllImport ("libc.so.6", EntryPoint = "abs")] static extern ushort AbsAsU2 (int v);
	[DllImport ("libc.so.6", EntryPoint = "toupper")] static extern char ToUpperAsChar (int c);
	[DllImport ("libc.so.6", EntryPoint = "strtoul")] static extern uint StrtoulAsU4 (string s, IntPtr end, int radix);
	[DllImport ("libc.so.6", EntryPoint = "strlen")] static extern UIntPtr StrlenAsNative (string s);
	[DllImport ("libc.so.6", EntryPoint = "div")] static extern Sig2DivOf<int> NativeDivOf (int a, int b);

	public static int test_1_pinvoke_sbyte_return ()
	{
		// abs (-200) is 200, whose low byte read as a signed one is -56.
		return AbsAsI1 (Id (-200)) == -56 && AbsAsI1 (Id (-5)) == 5 ? 1 : 0;
	}

	public static int test_1_pinvoke_short_return ()
	{
		// abs (-100000) is 100000, whose low word read as a signed one is -31072.
		return AbsAsI2 (Id (-100000)) == -31072 && AbsAsI2 (Id (-1000)) == 1000 ? 1 : 0;
	}

	// The same low word, read as an unsigned one.
	public static int test_34464_pinvoke_ushort_return ()
	{
		return AbsAsU2 (Id (-100000));
	}

	// MONO_TYPE_CHAR has a case label of its own, which the ushort return above
	// does not reach.
	public static int test_65_pinvoke_char_return ()
	{
		return ToUpperAsChar (Id ('a'));
	}

	// 3000000000 has bit 31 set, so an unsigned read and a signed one differ.
	public static int test_1_pinvoke_uint_return ()
	{
		return StrtoulAsU4 (IdS ("3000000000"), IntPtr.Zero, Id (10)) == 3000000000u ? 1 : 0;
	}

	public static int test_5_pinvoke_uintptr_return ()
	{
		return (int) (ulong) StrlenAsNative (IdS ("hello"));
	}

	// A generic instance asks the native layout for its size, which is the branch
	// a plain value type does not take. Both fields are read, so a size taken
	// from the wrong layout leaves the second one wrong.
	public static int test_1_pinvoke_generic_struct_return ()
	{
		Sig2DivOf<int> d = NativeDivOf (Id (17), Id (5));
		return d.Quot == 3 && d.Rem == 2 ? 1 : 0;
	}

	// The same conversion the other way, for the argument shapes that reach the
	// pinvoke branch of a case rather than the managed one.

	[DllImport ("libc.so.6", EntryPoint = "abs")] static extern int AbsFromStruct (Sig2Word v);
	[DllImport ("libc.so.6", EntryPoint = "abs")] static extern int AbsFromBox (Sig2Box<int> v);
	[DllImport ("libc.so.6", EntryPoint = "frexp")] static extern double NativeFrexp (double v, out int exponent);

	// A one-word struct goes in a register, so abs () reads the field.
	public static int test_11_pinvoke_struct_argument ()
	{
		return AbsFromStruct (new Sig2Word { Value = Id (-11) });
	}

	public static int test_12_pinvoke_generic_struct_argument ()
	{
		return AbsFromBox (new Sig2Box<int> { Value = Id (-12) });
	}

	// An out argument stays a byref through the marshaller, so it takes the byref
	// path rather than a case of the switch.
	public static int test_1_pinvoke_out_argument ()
	{
		int exponent;
		double mantissa = NativeFrexp (IdD (8.0), out exponent);
		return mantissa == 0.5 && exponent == 4 ? 1 : 0;
	}

	// A context static keeps the generic load and store, which convert through
	// the field's type. A pointer is the case no other field access reaches.
	public unsafe static int test_1_context_static_pointer ()
	{
		int first = Id (7), second = Id (9);

		Sig2Statics.Pointer = &first;
		int seen = *Sig2Statics.Pointer;
		Sig2Statics.Pointer = &second;
		return seen == 7 && *Sig2Statics.Pointer == 9 ? 1 : 0;
	}
}
