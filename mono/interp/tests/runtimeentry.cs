// Entering interpreted code from the runtime rather than from other IL.
//
// Reflection, Activator and DynamicInvoke all arrive through interp_entry,
// which converts each argument between the runtime's representation and the
// interpreter's stack one. That conversion has a case per type, so the tests
// here vary the signature rather than the body.

using System;
using System.Reflection;
using System.Runtime.CompilerServices;

public struct RuntimeEntryPair {
	public int First;
	public long Second;
}

public class RuntimeEntryTarget {

	public int Field;

	public RuntimeEntryTarget () { Field = 3; }
	public RuntimeEntryTarget (int seed) { Field = seed; }

	public static sbyte TakeI1 (sbyte x) { return (sbyte) (x + 1); }
	public static byte TakeU1 (byte x) { return (byte) (x + 1); }
	public static short TakeI2 (short x) { return (short) (x + 1); }
	public static ushort TakeU2 (ushort x) { return (ushort) (x + 1); }
	public static int TakeI4 (int x) { return x + 1; }
	public static uint TakeU4 (uint x) { return x + 1; }
	public static long TakeI8 (long x) { return x + 1; }
	public static ulong TakeU8 (ulong x) { return x + 1; }
	public static float TakeR4 (float x) { return x + 1; }
	public static double TakeR8 (double x) { return x + 1; }
	public static char TakeChar (char x) { return (char) (x + 1); }
	public static bool TakeBool (bool x) { return !x; }
	public static IntPtr TakeIntPtr (IntPtr x) { return (IntPtr) ((long) x + 1); }
	public static string TakeString (string x) { return x + "!"; }
	public static RuntimeEntryPair TakePair (RuntimeEntryPair p)
	{
		p.First++;
		p.Second++;
		return p;
	}

	public static void TakeNothing () { Marker = 41; }
	public static int Marker;

	public int Instance (int x) { return Field + x; }

	public static int Many (int a, long b, double c, string d, RuntimeEntryPair e)
	{
		return a + (int) b + (int) c + d.Length + e.First;
	}

	public static int Throws () { throw new InvalidOperationException (); }
}

public class RuntimeEntry {

	delegate int RuntimeEntryAdder (int a, int b);

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object Invoke (string name, params object [] args)
	{
		return typeof (RuntimeEntryTarget).GetMethod (name).Invoke (null, args);
	}

	public static int test_1_invoke_narrow_integers ()
	{
		return (sbyte) Invoke ("TakeI1", (sbyte) 1) == 2 &&
		       (byte) Invoke ("TakeU1", (byte) 1) == 2 &&
		       (short) Invoke ("TakeI2", (short) 1) == 2 &&
		       (ushort) Invoke ("TakeU2", (ushort) 1) == 2 ? 1 : 0;
	}

	public static int test_1_invoke_wide_integers ()
	{
		return (int) Invoke ("TakeI4", 1) == 2 &&
		       (uint) Invoke ("TakeU4", 1u) == 2u &&
		       (long) Invoke ("TakeI8", 1L) == 2L &&
		       (ulong) Invoke ("TakeU8", 1ul) == 2ul ? 1 : 0;
	}

	public static int test_1_invoke_floats ()
	{
		return (float) Invoke ("TakeR4", 1.5f) == 2.5f &&
		       (double) Invoke ("TakeR8", 1.5) == 2.5 ? 1 : 0;
	}

	public static int test_1_invoke_char_and_bool ()
	{
		return (char) Invoke ("TakeChar", 'a') == 'b' &&
		       (bool) Invoke ("TakeBool", false) ? 1 : 0;
	}

	public static int test_1_invoke_intptr ()
	{
		return (long) (IntPtr) Invoke ("TakeIntPtr", (IntPtr) 4) == 5L ? 1 : 0;
	}

	public static int test_1_invoke_reference ()
	{
		return (string) Invoke ("TakeString", "x") == "x!" ? 1 : 0;
	}

	public static int test_1_invoke_struct ()
	{
		RuntimeEntryPair p = new RuntimeEntryPair { First = 1, Second = 2 };
		RuntimeEntryPair r = (RuntimeEntryPair) Invoke ("TakePair", p);
		return r.First == 2 && r.Second == 3 ? 1 : 0;
	}

	public static int test_41_invoke_void ()
	{
		Invoke ("TakeNothing");
		return RuntimeEntryTarget.Marker;
	}

	public static int test_12_invoke_many_arguments ()
	{
		RuntimeEntryPair p = new RuntimeEntryPair { First = 4, Second = 0 };
		return (int) Invoke ("Many", 1, 2L, 3.0, "ab", p);
	}

	// An exception raised inside the invoked method arrives at the caller
	// wrapped, which is a different unwind path from an ordinary call.
	public static int test_1_invoke_throws ()
	{
		try {
			Invoke ("Throws");
			return 0;
		} catch (TargetInvocationException e) {
			return e.InnerException is InvalidOperationException ? 1 : 0;
		}
	}

	public static int test_7_invoke_instance_method ()
	{
		RuntimeEntryTarget t = new RuntimeEntryTarget (3);
		MethodInfo m = typeof (RuntimeEntryTarget).GetMethod ("Instance");
		return (int) m.Invoke (t, new object [] { 4 });
	}

	public static int test_3_activator_default_ctor ()
	{
		return ((RuntimeEntryTarget) Activator.CreateInstance (typeof (RuntimeEntryTarget))).Field;
	}

	public static int test_9_activator_with_argument ()
	{
		return ((RuntimeEntryTarget) Activator.CreateInstance (
			typeof (RuntimeEntryTarget), new object [] { 9 })).Field;
	}

	public static int test_5_dynamic_invoke ()
	{
		RuntimeEntryAdder add = (a, b) => a + b;
		return (int) add.DynamicInvoke (new object [] { 2, 3 });
	}

	public static int test_6_delegate_created_by_reflection ()
	{
		MethodInfo m = typeof (RuntimeEntryTarget).GetMethod ("TakeI4");
		Func<int, int> f = (Func<int, int>) Delegate.CreateDelegate (typeof (Func<int, int>), m);
		return f (5);
	}

	public static int test_4_field_read_by_reflection ()
	{
		RuntimeEntryTarget t = new RuntimeEntryTarget (4);
		return (int) typeof (RuntimeEntryTarget).GetField ("Field").GetValue (t);
	}

	public static int test_8_field_written_by_reflection ()
	{
		RuntimeEntryTarget t = new RuntimeEntryTarget (0);
		typeof (RuntimeEntryTarget).GetField ("Field").SetValue (t, 8);
		return t.Field;
	}

	public static int test_1_boxed_struct_round_trip ()
	{
		RuntimeEntryPair p = new RuntimeEntryPair { First = 6, Second = 7 };
		object boxed = p;
		RuntimeEntryPair back = (RuntimeEntryPair) boxed;
		return back.First == 6 && back.Second == 7 ? 1 : 0;
	}
}
