using System;
using System.Runtime.CompilerServices;

/*
 * The managed half of test-icall.cpp. Plain is registered with
 * mono_dangerous_add_internal_call_no_wrapper (), and Wrapped with the ordinary
 * mono_add_internal_call (), so the two arms differ only in the registration.
 */
public class Icalls
{
	[MethodImpl (MethodImplOptions.InternalCall)]
	public static extern int Plain (int x);

	[MethodImpl (MethodImplOptions.InternalCall)]
	public static extern int Wrapped (int x);

	public static int CallPlain (int x)
	{
		return Plain (x);
	}

	public static int CallWrapped (int x)
	{
		return Wrapped (x);
	}

	/*
	 * The control for the record test. NoInlining keeps this body out of its
	 * caller, so the call site really does name the method's entry and the
	 * domain really does hold a record for it.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int Managed (int x)
	{
		return x + 7;
	}

	public static int CallManaged (int x)
	{
		return Managed (x);
	}

	/*
	 * A try region whose only call is the icall. The icall is marked nounwind,
	 * so this method reaches codegen with no landing pad at all.
	 */
	public static int PlainInTry (int x)
	{
		try {
			return Plain (x);
		} catch (Exception) {
			return -1;
		}
	}

	/*
	 * A try region that also holds an operation which can throw. The clause
	 * keeps a protected range, and that range comes from the bounds check
	 * alone.
	 */
	public static int PlainInTryWithThrow (int x, int[] values)
	{
		try {
			return Plain (x) + values[4];
		} catch (IndexOutOfRangeException) {
			return -1;
		}
	}

	/* The assembly is built as an executable, and never run as one. */
	public static int Main ()
	{
		return 0;
	}
}
