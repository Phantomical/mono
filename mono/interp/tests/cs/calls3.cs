// The interpreter's fast path for a native call.
//
// A method named test_<n>_<what> is a test, and it passes when it returns <n>.
//
// A native signature whose arguments and result each fit one pointer-sized
// slot goes through one of fourteen fixed prototypes. The argument count and
// the return type pick the prototype. Everything else goes through the general
// native call, which builds a calling-convention context first. Each test here
// names one prototype, or one reason a signature is refused.
//
// A cdecl callee ignores arguments it does not declare, so one native function
// stands for several arities. The interpreter reads the declared signature. A
// memcmp declaration with three padding arguments is therefore a six-argument
// native call.
//
// The callees are the runner's own, in native.cpp.
//
// pinvoke.cs covers marshalling and the callback direction. These are about
// the dispatch.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

public class Calls3 {

	// The fourteen prototypes. Arity first, then void against non-void.

	[DllImport ("__Internal", EntryPoint = "interp_test_nothing")]
	static extern void Calls3Void0 ();

	[DllImport ("__Internal", EntryPoint = "interp_test_pagesize")]
	static extern int Calls3PageSize ();

	[DllImport ("__Internal", EntryPoint = "interp_test_note")]
	static extern void Calls3Note (int value);

	[DllImport ("__Internal", EntryPoint = "interp_test_toupper")]
	static extern int Calls3Upper (int c);

	[DllImport ("__Internal", EntryPoint = "interp_test_bzero")]
	static extern void Calls3Bzero (byte [] target, IntPtr count);

	[DllImport ("__Internal", EntryPoint = "interp_test_lookup")]
	static extern int Calls3Lookup (string name, int mode);

	[DllImport ("__Internal", EntryPoint = "interp_test_memcpy")]
	static extern void Calls3Copy3 (byte [] target, byte [] source, IntPtr count);

	[DllImport ("__Internal", EntryPoint = "interp_test_strncmp")]
	static extern int Calls3Ncmp (string a, string b, IntPtr count);

	[DllImport ("__Internal", EntryPoint = "interp_test_memcpy")]
	static extern void Calls3Copy4 (byte [] target, byte [] source, IntPtr count, int pad1);

	[DllImport ("__Internal", EntryPoint = "interp_test_memcmp")]
	static extern int Calls3Cmp4 (byte [] a, byte [] b, IntPtr count, int pad1);

	[DllImport ("__Internal", EntryPoint = "interp_test_memcpy")]
	static extern void Calls3Copy5 (byte [] target, byte [] source, IntPtr count,
	                                int pad1, int pad2);

	[DllImport ("__Internal", EntryPoint = "interp_test_memcmp")]
	static extern int Calls3Cmp5 (byte [] a, byte [] b, IntPtr count, int pad1, int pad2);

	[DllImport ("__Internal", EntryPoint = "interp_test_memcpy")]
	static extern void Calls3Copy6 (byte [] target, byte [] source, IntPtr count,
	                                int pad1, int pad2, int pad3);

	[DllImport ("__Internal", EntryPoint = "interp_test_memcmp")]
	static extern int Calls3Cmp6 (byte [] a, byte [] b, IntPtr count,
	                              int pad1, int pad2, int pad3);

	// The rest of what the fast path handles: a bool result, an enum argument
	// over either width, and a last error the caller reads afterwards.

	[DllImport ("__Internal", EntryPoint = "interp_test_lookup")]
	static extern bool Calls3LookupFails (string name, int mode);

	[DllImport ("__Internal", EntryPoint = "interp_test_abs")]
	static extern int Calls3AbsEnum (Calls3Mode value);

	[DllImport ("__Internal", EntryPoint = "interp_test_abs")]
	static extern int Calls3AbsWide (Calls3Wide value);

	[DllImport ("__Internal", EntryPoint = "interp_test_lookup", SetLastError = true)]
	static extern int Calls3LookupErrno (string name, int mode);

	// Shapes the fast path refuses. Each still has to call.

	[DllImport ("__Internal", EntryPoint = "interp_test_abs")]
	static extern byte Calls3AbsByte (int value);

	[DllImport ("__Internal", EntryPoint = "interp_test_abs")]
	static extern short Calls3AbsShort (int value);

	[DllImport ("__Internal", EntryPoint = "interp_test_nothing")]
	static extern void Calls3Void1 (double ignored);

	[DllImport ("__Internal", EntryPoint = "interp_test_nothing")]
	static extern void Calls3Void2 (double a, double b);

	[DllImport ("__Internal", EntryPoint = "interp_test_abs")]
	static extern int Calls3AbsThen (int value, double ignored);

	[DllImport ("__Internal", EntryPoint = "interp_test_abs")]
	static extern int Calls3AbsLast6 (int value, int p1, int p2, int p3, int p4, double last);

	[DllImport ("__Internal", EntryPoint = "interp_test_abs")]
	static extern int Calls3AbsPair (Calls3Pair pair);

	public enum Calls3Mode { Zero = 0, MinusSeven = -7 }

	public enum Calls3Wide : long { Zero = 0, MinusSeven = -7 }

	[StructLayout (LayoutKind.Sequential)]
	public struct Calls3Pair {
		public int First;
		public int Second;
	}

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double IdD (double x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static string IdS (string x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static byte [] Bytes (int a, int b, int c, int d)
	{
		return new byte [] { (byte) a, (byte) b, (byte) c, (byte) d };
	}

	// No arguments and no result. Nothing comes back, so the test is that the
	// call returns and the next statement runs.

	public static int test_1_no_arguments_no_result ()
	{
		Calls3Void0 ();
		return Id (1);
	}

	// The callee answers 4096, so the result is the answer and a slot it did
	// not write cannot pass.

	public static int test_4096_no_arguments_with_result ()
	{
		return Calls3PageSize ();
	}

	public static int test_1_one_argument_no_result ()
	{
		Calls3Note (Id (12345));
		return Id (1);
	}

	// Upper case 'a' is 'A', which is 65.

	public static int test_65_one_argument_with_result ()
	{
		return Calls3Upper (Id ('a'));
	}

	// The callee writes through the array it is given, so the buffer is what
	// the test reads.

	public static int test_1_two_arguments_no_result ()
	{
		byte [] buffer = Bytes (9, 9, 9, 9);
		Calls3Bzero (buffer, (IntPtr) Id (3));
		return buffer [0] == 0 && buffer [2] == 0 && buffer [3] == 9 ? 1 : 0;
	}

	// Both answers are asked for. A zero on its own is also what an untouched
	// result slot holds.

	public static int test_1_two_arguments_with_result ()
	{
		return Calls3Lookup (IdS ("abs"), Id (0)) == 0
			&& Calls3Lookup (IdS ("no_such_function"), Id (0)) == -1 ? 1 : 0;
	}

	public static int test_1_three_arguments_no_result ()
	{
		byte [] source = Bytes (1, 2, 3, 4);
		byte [] target = Bytes (0, 0, 0, 0);
		Calls3Copy3 (target, source, (IntPtr) Id (4));
		return target [0] == 1 && target [3] == 4 ? 1 : 0;
	}

	// "hello" and "help" agree over three bytes and part at the fourth, so the
	// count decides the answer as well as the two strings do.

	public static int test_1_three_arguments_with_result ()
	{
		return Calls3Ncmp (IdS ("hello"), IdS ("help"), (IntPtr) Id (3)) == 0
			&& Calls3Ncmp (IdS ("hello"), IdS ("help"), (IntPtr) Id (4)) < 0 ? 1 : 0;
	}

	// From here the declaration carries arguments the callee never reads. The
	// count still picks the prototype, so the interpreter must lay them out.

	public static int test_1_four_arguments_no_result ()
	{
		byte [] source = Bytes (5, 6, 7, 8);
		byte [] target = Bytes (0, 0, 0, 0);
		Calls3Copy4 (target, source, (IntPtr) Id (4), Id (11));
		return target [0] == 5 && target [3] == 8 ? 1 : 0;
	}

	public static int test_1_four_arguments_with_result ()
	{
		byte [] a = Bytes (1, 2, 3, 4);
		byte [] b = Bytes (1, 2, 3, 5);
		return Calls3Cmp4 (a, a, (IntPtr) Id (4), Id (11)) == 0
			&& Calls3Cmp4 (a, b, (IntPtr) Id (4), Id (11)) < 0 ? 1 : 0;
	}

	public static int test_1_five_arguments_no_result ()
	{
		byte [] source = Bytes (5, 6, 7, 8);
		byte [] target = Bytes (0, 0, 0, 0);
		Calls3Copy5 (target, source, (IntPtr) Id (4), Id (11), Id (12));
		return target [0] == 5 && target [3] == 8 ? 1 : 0;
	}

	public static int test_1_five_arguments_with_result ()
	{
		byte [] a = Bytes (1, 2, 3, 4);
		byte [] b = Bytes (1, 2, 3, 5);
		return Calls3Cmp5 (a, b, (IntPtr) Id (4), Id (11), Id (12)) < 0 ? 1 : 0;
	}

	public static int test_1_six_arguments_no_result ()
	{
		byte [] source = Bytes (5, 6, 7, 8);
		byte [] target = Bytes (0, 0, 0, 0);
		Calls3Copy6 (target, source, (IntPtr) Id (4), Id (11), Id (12), Id (13));
		return target [0] == 5 && target [3] == 8 ? 1 : 0;
	}

	public static int test_1_six_arguments_with_result ()
	{
		byte [] a = Bytes (1, 2, 3, 4);
		byte [] b = Bytes (1, 2, 3, 3);
		return Calls3Cmp6 (a, b, (IntPtr) Id (4), Id (11), Id (12), Id (13)) > 0 ? 1 : 0;
	}

	// The remaining types the fast path takes beside int: a bool result and an
	// enum argument over either width.

	// The callee returns -1 for a failure and 0 for success. The interpreter
	// reads a bool result from the low byte of what it returned.

	public static int test_1_bool_result ()
	{
		return Calls3LookupFails (IdS ("no_such_function"), Id (0))
			&& !Calls3LookupFails (IdS ("abs"), Id (0)) ? 1 : 0;
	}

	public static int test_7_enum_argument ()
	{
		Calls3Mode mode = Calls3Mode.MinusSeven;
		return Calls3AbsEnum (mode);
	}

	// The callee reads the low half of the register it is given, so an enum
	// over long gets the same answer as one over int.

	public static int test_7_long_enum_argument ()
	{
		Calls3Wide wide = Calls3Wide.MinusSeven;
		return Calls3AbsWide (wide);
	}

	// The caller reads the last error after the call comes back, so the fast
	// path has to keep it. The callee sets ENOENT, which is 2.

	public static int test_2_last_error_is_kept ()
	{
		if (Calls3LookupErrno (IdS ("no_such_function"), Id (0)) != -1)
			return 0;
		return Marshal.GetLastWin32Error ();
	}

	// A floating point value does not fit a prototype, and neither does a
	// result narrower than a slot. These calls take the general native call.
	// Each one still has to reach the callee.

	public static int test_7_byte_result_is_refused ()
	{
		return Calls3AbsByte (Id (-7));
	}

	public static int test_7_short_result_is_refused ()
	{
		return Calls3AbsShort (Id (-7));
	}

	public static int test_1_double_argument_is_refused ()
	{
		Calls3Void1 (IdD (2.5));
		return Id (1);
	}

	public static int test_1_two_double_arguments_are_refused ()
	{
		Calls3Void2 (IdD (2.5), IdD (3.5));
		return Id (1);
	}

	public static int test_7_second_argument_is_refused ()
	{
		return Calls3AbsThen (Id (-7), IdD (2.5));
	}

	public static int test_7_sixth_argument_is_refused ()
	{
		return Calls3AbsLast6 (Id (-7), Id (1), Id (2), Id (3), Id (4), IdD (2.5));
	}

	public static int test_7_struct_argument_is_refused ()
	{
		Calls3Pair pair;
		pair.First = Id (-7);
		pair.Second = Id (3);
		return Calls3AbsPair (pair);
	}
}
