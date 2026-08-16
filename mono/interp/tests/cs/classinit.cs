// Class initializers and the static kinds.
//
// An ordinary static access carries the vtable, so the transform can make the
// initializer run before the load. A [ThreadStatic] or [ContextStatic] field is
// reached by offset instead, which is the case worth separating out.

using System;
using System.Runtime.CompilerServices;

public class ClassInitOrdinary {
	public static int Value;
	static ClassInitOrdinary () { Value = 21; }
}

public class ClassInitThreadStatic {
	[ThreadStatic] public static int Value;
	static ClassInitThreadStatic () { Value = 22; }
}

public class ClassInitContextStatic {
	[ContextStatic] public static int Value;
	static ClassInitContextStatic () { Value = 23; }
}

public class ClassInitReadOnly {
	public static readonly int Frozen = 24;
	public static int Mutable = 25;
}

public class ClassInitOrder {
	public static string Log = "";
	public static int Touched;
	static ClassInitOrder () { Log += "cctor;"; Touched = 1; }
	public static int Read () { return Touched; }
}

public class ClassInitThrows {
	public static int Value;
	static ClassInitThrows () { throw new InvalidOperationException (); }
}

public struct ClassInitStruct {
	public static int Value;
	static ClassInitStruct () { Value = 26; }
	public int Instance;
}

public class ClassInitGeneric<T> {
	public static int Value;
	static ClassInitGeneric () { Value = 27; }
}

public class ClassInit {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }

	public static int test_21_ordinary_static_runs_the_cctor ()
	{
		return ClassInitOrdinary.Value;
	}

	// The declaring class is touched for the first time from here, and its
	// initializer is what puts a value in the field.
	public static int test_22_thread_static_runs_the_cctor ()
	{
		return ClassInitThreadStatic.Value;
	}

	public static int test_23_context_static_runs_the_cctor ()
	{
		return ClassInitContextStatic.Value;
	}

	public static int test_26_struct_static_runs_the_cctor ()
	{
		return ClassInitStruct.Value;
	}

	public static int test_27_generic_static_runs_the_cctor ()
	{
		return ClassInitGeneric<string>.Value;
	}

	public static int test_1_generic_statics_are_per_instantiation ()
	{
		ClassInitGeneric<int>.Value = 5;
		ClassInitGeneric<long>.Value = 6;
		return ClassInitGeneric<int>.Value == 5 && ClassInitGeneric<long>.Value == 6 ? 1 : 0;
	}

	public static int test_24_static_readonly_reads ()
	{
		return ClassInitReadOnly.Frozen;
	}

	public static int test_1_cctor_runs_once ()
	{
		ClassInitOrder.Read ();
		ClassInitOrder.Read ();
		int unused = ClassInitOrder.Touched;
		return ClassInitOrder.Log == "cctor;" ? 1 : 0;
	}

	public static int test_1_failing_cctor_throws_type_initialization ()
	{
		try {
			return ClassInitThrows.Value == 0 ? 0 : 0;
		} catch (TypeInitializationException e) {
			return e.InnerException is InvalidOperationException ? 1 : 0;
		}
	}

	// The second touch of a class whose initializer threw gets the same
	// exception without running anything again.
	public static int test_2_failing_cctor_stays_failed ()
	{
		int seen = 0;
		for (int i = 0; i < 2; i++) {
			try {
				ClassInitThrows.Value = Id (1);
			} catch (TypeInitializationException) {
				seen++;
			}
		}
		return seen;
	}

	// The initializer runs once, on whichever thread touched the class first, so
	// a [ThreadStatic] field it wrote is set on that thread only.
	public static int test_1_thread_static_is_per_thread ()
	{
		ClassInitThreadStatic.Value = 5;
		int other = -1;
		System.Threading.Thread t = new System.Threading.Thread (
			() => other = ClassInitThreadStatic.Value);
		t.Start ();
		t.Join ();
		return ClassInitThreadStatic.Value == 5 && other == 0 ? 1 : 0;
	}

	public static int test_25_ordinary_static_is_writable ()
	{
		ClassInitReadOnly.Mutable = 25;
		return ClassInitReadOnly.Mutable;
	}

	public static int test_1_static_field_address ()
	{
		unsafe {
			fixed (int *p = &ClassInitOrdinary.Value) {
				*p = 30;
			}
		}
		return ClassInitOrdinary.Value == 30 ? 1 : 0;
	}
}
