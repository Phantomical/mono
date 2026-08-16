// Leaving the interpreter for native code and coming back.
//
// A P/Invoke goes through a marshalling wrapper, which is always compiled, so
// these tests cross the engine boundary twice per call whatever tier the caller
// is in.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

public class PInvoke {

	[DllImport ("__Internal", EntryPoint = "interp_test_abs")]
	static extern int NativeAbs (int value);

	[DllImport ("__Internal", EntryPoint = "interp_test_labs")]
	static extern long NativeLabs (long value);

	[DllImport ("__Internal", EntryPoint = "interp_test_strlen")]
	static extern IntPtr NativeStrlen (string s);

	[DllImport ("__Internal", EntryPoint = "interp_test_memcmp")]
	static extern int NativeMemcmp (byte [] a, byte [] b, IntPtr count);

	[DllImport ("__Internal", EntryPoint = "interp_test_memset")]
	static extern IntPtr NativeMemset (byte [] target, int value, IntPtr count);

	[DllImport ("__Internal", EntryPoint = "interp_test_atof")]
	static extern double NativeAtof (string s);

	[DllImport ("__Internal", EntryPoint = "interp_test_next_id")]
	static extern int NativeNextId ();

	[DllImport ("__Internal", EntryPoint = "interp_test_format")]
	static extern int NativeSnprintf (StringBuilder buffer, IntPtr size, string format, int a);

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }

	public static int test_7_pinvoke_int_argument ()
	{
		return NativeAbs (Id (-7));
	}

	public static int test_1_pinvoke_long_argument ()
	{
		return NativeLabs (-0x100000001L) == 0x100000001L ? 1 : 0;
	}

	public static int test_5_pinvoke_string_argument ()
	{
		return (int) NativeStrlen ("hello");
	}

	public static int test_0_pinvoke_no_arguments ()
	{
		// The id counts up, so the test is that the call returns at all.
		return NativeNextId () > 0 ? 0 : 1;
	}

	public static int test_1_pinvoke_double_return ()
	{
		return NativeAtof ("2.5") == 2.5 ? 1 : 0;
	}

	public static int test_1_pinvoke_byte_array ()
	{
		byte [] a = { 1, 2, 3 };
		byte [] b = { 1, 2, 3 };
		byte [] c = { 1, 2, 4 };
		return NativeMemcmp (a, b, (IntPtr) 3) == 0 &&
		       NativeMemcmp (a, c, (IntPtr) 3) != 0 ? 1 : 0;
	}

	public static int test_1_pinvoke_writes_back ()
	{
		byte [] buffer = new byte [4];
		NativeMemset (buffer, 0x41, (IntPtr) 4);
		return buffer [0] == 0x41 && buffer [3] == 0x41 ? 1 : 0;
	}

	public static int test_2_pinvoke_stringbuilder ()
	{
		StringBuilder sb = new StringBuilder (16);
		NativeSnprintf (sb, (IntPtr) 16, "%d", Id (42));
		return sb.ToString () == "42" ? 2 : 0;
	}

	// A missing entry point is reported when the call is made, not when the
	// method is transformed.
	[DllImport ("__Internal", EntryPoint = "interp_test_no_such_function")]
	static extern int NativeMissing ();

	public static int test_1_missing_entry_point_throws ()
	{
		try {
			NativeMissing ();
			return 0;
		} catch (EntryPointNotFoundException) {
			return 1;
		}
	}

	// Marshal's own surface reaches the same native memory the interpreter's
	// pointer opcodes do.
	public static int test_9_marshal_round_trip ()
	{
		IntPtr block = Marshal.AllocHGlobal (8);
		try {
			Marshal.WriteInt32 (block, 9);
			return Marshal.ReadInt32 (block);
		} finally {
			Marshal.FreeHGlobal (block);
		}
	}

	public static int test_1_marshal_string_round_trip ()
	{
		IntPtr native = Marshal.StringToHGlobalAnsi ("round");
		try {
			return Marshal.PtrToStringAnsi (native) == "round" ? 1 : 0;
		} finally {
			Marshal.FreeHGlobal (native);
		}
	}

	public static int test_8_marshal_sizeof ()
	{
		return Marshal.SizeOf (typeof (long)) + Marshal.SizeOf (typeof (int)) == 12 ? 8 : 0;
	}

	// Native code calling back into managed code. The callback is entered
	// through a calling-convention context rather than from other IL, which is
	// its own path into the interpreter.

	delegate int PInvokeCompare (IntPtr a, IntPtr b);

	[DllImport ("__Internal", EntryPoint = "interp_test_sort")]
	static extern void NativeQsort (int [] items, IntPtr count, IntPtr size,
	                                PInvokeCompare compare);

	static int Ascending (IntPtr a, IntPtr b)
	{
		return Marshal.ReadInt32 (a) - Marshal.ReadInt32 (b);
	}

	public static int test_1_native_calls_back_into_managed ()
	{
		int [] items = { 5, 3, 4, 1, 2 };
		NativeQsort (items, (IntPtr) items.Length, (IntPtr) 4, Ascending);
		for (int i = 0; i < items.Length; i++)
			if (items [i] != i + 1)
				return 0;
		return 1;
	}

	delegate int PInvokeAdder (int a, int b);

	static int AddTwo (int a, int b) { return a + b; }

	public static int test_7_function_pointer_round_trip ()
	{
		PInvokeAdder managed = AddTwo;
		IntPtr raw = Marshal.GetFunctionPointerForDelegate (managed);
		PInvokeAdder back = (PInvokeAdder) Marshal.GetDelegateForFunctionPointer (
			raw, typeof (PInvokeAdder));
		return back (3, 4);
	}

	public static int test_1_callback_that_throws ()
	{
		PInvokeAdder managed = (a, b) => throw new InvalidOperationException ();
		IntPtr raw = Marshal.GetFunctionPointerForDelegate (managed);
		PInvokeAdder back = (PInvokeAdder) Marshal.GetDelegateForFunctionPointer (
			raw, typeof (PInvokeAdder));
		try {
			back (1, 2);
			return 0;
		} catch (InvalidOperationException) {
			return 1;
		}
	}
}
