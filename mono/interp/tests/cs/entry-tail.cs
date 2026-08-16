// Delegates and reflection as ways in to a P/Invoke method.
//
// A delegate over a [DllImport] method makes the interpreter mint an entry
// address for a method whose body is a marshalling wrapper. That is the pinvoke
// arm of interp_create_method_pointer. delegates.cs already reaches that arm
// with an int-to-int signature, so what is left is how the delegate gets bound,
// and the entry cache on InterpMethod.
//
// The rest of the file is a two-int struct the native callee returns in a
// register pair. Six routes reach that one native call. Four give both words
// back; the two that go through reflection lose the remainder. Both engines
// lose it, so that is a marshalling defect rather than a tier seam.
//
// A method named test_<n>_<what> is a test, and it passes when it returns <n>.
// Operands go through the NoInlining Id helper, so the transform cannot fold a
// test into its answer.

using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

// The pair interp_test_div () returns by value. Two ints, so the result comes
// back in one register rather than through a return buffer. objcopy.cs uses the
// same struct for the direct call.
[StructLayout (LayoutKind.Sequential)]
public struct EntryTailDiv {
	public int Quotient;
	public int Remainder;
}

[Instrumented]
public class EntryTail {

	[DllImport ("__Internal", EntryPoint = "interp_test_abs")]
	public static extern int NativeAbs (int value);

	[DllImport ("__Internal", EntryPoint = "interp_test_div")]
	public static extern EntryTailDiv NativeDiv (int numerator, int denominator);

	[DllImport ("__Internal", EntryPoint = "interp_test_no_such_function")]
	public static extern int NativeMissing (int value);

	delegate int EntryTailAbsFunc (int value);
	delegate EntryTailDiv EntryTailDivFunc (int numerator, int denominator);

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Id (int x) { return x; }

	static int PlusSix (int x) { return x + 6; }

	// The same arithmetic, from a managed body.
	public static EntryTailDiv ManagedDiv (int numerator, int denominator)
	{
		return new EntryTailDiv {
			Quotient = numerator / denominator,
			Remainder = numerator % denominator
		};
	}

	// Building the entry for a P/Invoke method.

	// The address is kept on the InterpMethod, so the second delegate finds the
	// one the first minted rather than asking for another.
	public static int test_10_two_delegates_over_one_pinvoke ()
	{
		EntryTailAbsFunc first = NativeAbs;
		EntryTailAbsFunc second = NativeAbs;
		return first (Id (-4)) + second (Id (-6));
	}

	// The bytecode binds the delegates above. Here the runtime binds one, from
	// the same method, and the entry has to come out the same.
	public static int test_7_createdelegate_over_pinvoke ()
	{
		MethodInfo mi = typeof (EntryTail).GetMethod ("NativeAbs");
		EntryTailAbsFunc f = (EntryTailAbsFunc) Delegate.CreateDelegate (
			typeof (EntryTailAbsFunc), mi);
		return f (Id (-7));
	}

	// One delegate type, both arms of the decision. The native target takes the
	// pinvoke arm and the managed one takes the LMF-wrapper arm.
	public static int test_17_pinvoke_and_managed_share_a_delegate_type ()
	{
		EntryTailAbsFunc native = NativeAbs;
		EntryTailAbsFunc managed = PlusSix;
		return native (Id (-7)) + managed (Id (4));
	}

	// Minting the entry does not resolve the native symbol. The missing entry
	// point is still reported when the call is made.
	public static int test_1_delegate_over_missing_pinvoke_throws ()
	{
		EntryTailAbsFunc f = NativeMissing;
		try {
			f (Id (1));
			return 0;
		} catch (EntryPointNotFoundException) {
			return 1;
		}
	}

	/*
	 * Six routes to div (7, 2), whose quotient is 3 and whose remainder is 1.
	 * Each test adds the two fields, so a lost remainder reads as 3.
	 *
	 *   through a delegate                              4
	 *   MethodInfo.Invoke of the delegate's Invoke       4
	 *   MethodInfo.Invoke of a managed method            4
	 *   MethodInfo.Invoke of the P/Invoke method         3
	 *   DynamicInvoke of the delegate                    3
	 *
	 * objcopy.cs holds the sixth route, the direct call, and it reads both
	 * fields. So neither the native return nor reflection's handling of a
	 * returned struct is what drops the word. The two failing routes are the
	 * ones that enter the P/Invoke method itself through the runtime invoke
	 * path.
	 */

	public static int test_4_delegate_over_pinvoke_struct_return ()
	{
		EntryTailDivFunc f = NativeDiv;
		EntryTailDiv d = f (Id (7), Id (2));
		return d.Quotient + d.Remainder;
	}

	// One frame further out: reflection enters Invoke, and Invoke makes the
	// native call from bytecode.
	public static int test_4_reflection_invoke_of_a_delegate_over_a_pinvoke ()
	{
		EntryTailDivFunc f = NativeDiv;
		MethodInfo mi = typeof (EntryTailDivFunc).GetMethod ("Invoke");
		EntryTailDiv d = (EntryTailDiv) mi.Invoke (f, new object [] { Id (7), Id (2) });
		return d.Quotient + d.Remainder;
	}

	// The same struct and the same reflection call, over a managed body.
	// runtimeentry.cs invokes a 16-byte struct this way; this one is 8 bytes and
	// comes back in a single register.
	public static int test_4_reflection_invoke_of_managed_struct_return ()
	{
		MethodInfo mi = typeof (EntryTail).GetMethod ("ManagedDiv");
		EntryTailDiv d = (EntryTailDiv) mi.Invoke (null, new object [] { Id (7), Id (2) });
		return d.Quotient + d.Remainder;
	}

	// A defect both engines share, so it is not a tier seam: the remainder half
	// of the struct is lost. Calling the same P/Invoke directly gives 4.
	public static int test_4_reflection_invoke_of_pinvoke_struct_return ()
	{
		MethodInfo mi = typeof (EntryTail).GetMethod ("NativeDiv");
		EntryTailDiv d = (EntryTailDiv) mi.Invoke (null, new object [] { Id (7), Id (2) });
		return d.Quotient + d.Remainder;
	}

	// DynamicInvoke reaches the target method rather than Invoke, so it lands on
	// the same path as the test above and loses the same half.
	public static int test_4_dynamic_invoke_over_pinvoke_struct_return ()
	{
		EntryTailDivFunc f = NativeDiv;
		EntryTailDiv d = (EntryTailDiv) f.DynamicInvoke (new object [] { Id (7), Id (2) });
		return d.Quotient + d.Remainder;
	}

	// The same two routes with a return that fits a word, which both give.

	public static int test_7_reflection_invoke_of_pinvoke ()
	{
		MethodInfo mi = typeof (EntryTail).GetMethod ("NativeAbs");
		return (int) mi.Invoke (null, new object [] { Id (-7) });
	}

	public static int test_7_dynamic_invoke_over_pinvoke ()
	{
		EntryTailAbsFunc f = NativeAbs;
		return (int) f.DynamicInvoke (new object [] { Id (-7) });
	}
}
