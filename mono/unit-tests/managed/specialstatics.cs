// [ThreadStatic] and [ContextStatic] fields.
//
// A special static is reached by an offset into per-thread storage rather than
// through the class's vtable, and the interpreter has a separate opcode per
// width for both the load and the store.

using System;
using System.Runtime.CompilerServices;
using System.Threading;

public class SpecialStaticHolder {
	[ThreadStatic] public static sbyte I1;
	[ThreadStatic] public static byte U1;
	[ThreadStatic] public static short I2;
	[ThreadStatic] public static ushort U2;
	[ThreadStatic] public static int I4;
	[ThreadStatic] public static long I8;
	[ThreadStatic] public static float R4;
	[ThreadStatic] public static double R8;
	[ThreadStatic] public static string Reference;
	[ThreadStatic] public static SpecialStaticPair Value;
	[ThreadStatic] public static SpecialStaticFlags Flags;
}

public class SpecialStaticContext {
	[ContextStatic] public static int I4;
	[ContextStatic] public static string Reference;
	[ContextStatic] public static SpecialStaticPair Value;
}

public struct SpecialStaticPair {
	public int First;
	public long Second;
}

public enum SpecialStaticFlags : byte {
	None = 0,
	One = 1,
	Two = 2,
}

public class SpecialStatics {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long IdL (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double IdD (double x) { return x; }

	public static int test_1_thread_static_narrow_integers ()
	{
		SpecialStaticHolder.I1 = (sbyte) Id (-8);
		SpecialStaticHolder.U1 = (byte) Id (200);
		SpecialStaticHolder.I2 = (short) Id (-300);
		SpecialStaticHolder.U2 = (ushort) Id (60000);
		return SpecialStaticHolder.I1 == -8 && SpecialStaticHolder.U1 == 200 &&
		       SpecialStaticHolder.I2 == -300 && SpecialStaticHolder.U2 == 60000 ? 1 : 0;
	}

	public static int test_1_thread_static_wide_integers ()
	{
		SpecialStaticHolder.I4 = Id (-70000);
		SpecialStaticHolder.I8 = IdL (0x1_0000_0001L);
		return SpecialStaticHolder.I4 == -70000 &&
		       SpecialStaticHolder.I8 == 0x1_0000_0001L ? 1 : 0;
	}

	public static int test_1_thread_static_floats ()
	{
		SpecialStaticHolder.R4 = (float) IdD (1.5);
		SpecialStaticHolder.R8 = IdD (2.5);
		return SpecialStaticHolder.R4 == 1.5f && SpecialStaticHolder.R8 == 2.5 ? 1 : 0;
	}

	public static int test_5_thread_static_reference ()
	{
		SpecialStaticHolder.Reference = "hello";
		GC.Collect ();
		return SpecialStaticHolder.Reference.Length;
	}

	public static int test_1_thread_static_value_type ()
	{
		SpecialStaticHolder.Value = new SpecialStaticPair { First = Id (3), Second = IdL (4) };
		SpecialStaticPair read = SpecialStaticHolder.Value;
		return read.First == 3 && read.Second == 4 ? 1 : 0;
	}

	public static int test_2_thread_static_enum ()
	{
		SpecialStaticHolder.Flags = SpecialStaticFlags.Two;
		return (int) SpecialStaticHolder.Flags;
	}

	public static int test_1_thread_static_address ()
	{
		SpecialStaticHolder.I4 = 0;
		unsafe {
			fixed (int *p = &SpecialStaticHolder.I4)
				*p = 12;
		}
		return SpecialStaticHolder.I4 == 12 ? 1 : 0;
	}

	// The address of a value-type special static, taken so the test can write a
	// field through it.
	public static int test_1_thread_static_value_type_field_write ()
	{
		SpecialStaticHolder.Value = default;
		SetFirst (ref SpecialStaticHolder.Value, Id (9));
		return SpecialStaticHolder.Value.First == 9 ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void SetFirst (ref SpecialStaticPair p, int value) { p.First = value; }

	public static int test_1_thread_static_is_zero_on_another_thread ()
	{
		SpecialStaticHolder.I4 = Id (7);
		int other = -1;
		Thread t = new Thread (() => other = SpecialStaticHolder.I4);
		t.Start ();
		t.Join ();
		return SpecialStaticHolder.I4 == 7 && other == 0 ? 1 : 0;
	}

	public static int test_1_thread_static_reference_is_per_thread ()
	{
		SpecialStaticHolder.Reference = "mine";
		string other = "unset";
		Thread t = new Thread (() => other = SpecialStaticHolder.Reference);
		t.Start ();
		t.Join ();
		return SpecialStaticHolder.Reference == "mine" && other == null ? 1 : 0;
	}

	public static int test_6_context_static_integer ()
	{
		SpecialStaticContext.I4 = Id (6);
		return SpecialStaticContext.I4;
	}

	public static int test_4_context_static_reference ()
	{
		SpecialStaticContext.Reference = "four";
		return SpecialStaticContext.Reference.Length;
	}

	public static int test_1_context_static_value_type ()
	{
		SpecialStaticContext.Value = new SpecialStaticPair { First = Id (1), Second = IdL (2) };
		return SpecialStaticContext.Value.First == 1 &&
		       SpecialStaticContext.Value.Second == 2 ? 1 : 0;
	}

	public static int test_1_special_static_survives_a_collection ()
	{
		SpecialStaticHolder.Reference = new string ('x', 8);
		SpecialStaticContext.Reference = new string ('y', 8);
		GC.Collect ();
		GC.WaitForPendingFinalizers ();
		return SpecialStaticHolder.Reference.Length == 8 &&
		       SpecialStaticContext.Reference.Length == 8 ? 1 : 0;
	}

	public static int test_3_repeated_access_reads_one_slot ()
	{
		SpecialStaticHolder.I4 = 0;
		for (int i = 0; i < 3; i++)
			SpecialStaticHolder.I4 = SpecialStaticHolder.I4 + 1;
		return SpecialStaticHolder.I4;
	}
}
