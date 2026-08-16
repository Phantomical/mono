// Which callees the transform inlines, and what an inlined body has to keep
// doing correctly.
//
// The decision is made in interp_method_check_inlining, and it turns on the
// callee's IL size, its flags, its clauses and its class. Each test here calls
// a callee of one shape and checks the answer, so a wrong inline shows up as a
// wrong number rather than as a missing optimization.

using System;
using System.Runtime.CompilerServices;

public class InliningMarshalByRef : MarshalByRefObject {
	public int Small () { return 5; }
}

public class InliningLateInit {
	public static int Seed = 7;
	static InliningLateInit () { Seed = 8; }
	public static int Small () { return Seed; }
}

public class Inlining {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }

	// Under the size limit and with no clauses: the body is inlined.
	static int Tiny (int x) { return x + 1; }

	public static int test_4_tiny_callee ()
	{
		return Tiny (Id (3));
	}

	public static int test_6_tiny_callee_twice ()
	{
		return Tiny (Tiny (Id (4)));
	}

	// Chained calls put the inliner several levels deep.
	static int Level1 (int x) { return Level2 (x) + 1; }
	static int Level2 (int x) { return Level3 (x) + 1; }
	static int Level3 (int x) { return Level4 (x) + 1; }
	static int Level4 (int x) { return x + 1; }

	public static int test_14_nested_inlines ()
	{
		return Level1 (Id (10));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Refused (int x) { return x + 1; }

	public static int test_4_noinlining_callee ()
	{
		return Refused (Id (3));
	}

	[MethodImpl (MethodImplOptions.AggressiveInlining)]
	static int BigButAggressive (int x)
	{
		int a = x + 1, b = x + 2, c = x + 3, d = x + 4, e = x + 5;
		int f = a + b, g = c + d, h = e + a, i = b + c, j = d + e;
		return (f + g + h + i + j) % 97 + x;
	}

	public static int test_1_aggressive_inlining_callee ()
	{
		return BigButAggressive (Id (3)) == BigButAggressive (3) ? 1 : 0;
	}

	// A callee with a clause of its own is never inlined.
	static int HasClauses (int x)
	{
		try {
			return x + 1;
		} catch (Exception) {
			return -1;
		}
	}

	public static int test_4_callee_with_clauses ()
	{
		return HasClauses (Id (3));
	}

	[MethodImpl (MethodImplOptions.Synchronized)]
	static int Synchronized (int x) { return x + 1; }

	public static int test_4_synchronized_callee ()
	{
		return Synchronized (Id (3));
	}

	public static int test_5_marshalbyref_callee ()
	{
		return new InliningMarshalByRef ().Small ();
	}

	// The class needs its initializer run, so the first call cannot be inlined
	// and a later one can.
	public static int test_16_callee_whose_class_needs_its_cctor ()
	{
		return InliningLateInit.Small () + InliningLateInit.Small ();
	}

	// An inlined body still has its own locals.
	static int UsesLocals (int x)
	{
		int a = x * 2;
		int b = a + 1;
		return b;
	}

	public static int test_7_inlined_locals_do_not_alias ()
	{
		int a = Id (100);
		int b = UsesLocals (Id (3));
		return b - (a - 100);
	}

	static InliningPair MakesStruct (int x)
	{
		return new InliningPair { First = x, Second = x + 1 };
	}

	public static int test_7_inlined_struct_return ()
	{
		InliningPair p = MakesStruct (Id (3));
		return p.First + p.Second;
	}

	static int Branchy (int x)
	{
		if (x > 0)
			return 1;
		return 2;
	}

	public static int test_3_inlined_branches ()
	{
		return Branchy (Id (1)) + Branchy (Id (-1));
	}

	// A recursive callee: the inliner has a depth limit rather than a cycle
	// check, so this has to terminate on its own.
	static int Recurse (int n) { return n == 0 ? 0 : Recurse (n - 1) + 1; }

	public static int test_6_recursive_callee ()
	{
		return Recurse (Id (6));
	}

	static int Argless () { return 12; }

	public static int test_12_argless_callee ()
	{
		return Argless ();
	}

	static void Void (int x) { InliningSink = x; }
	static int InliningSink;

	public static int test_9_void_callee ()
	{
		Void (Id (9));
		return InliningSink;
	}

	// A virtual call is not inlined unless the site is already resolved.
	public static int test_3_virtual_callee ()
	{
		InliningBase b = new InliningDerived ();
		return b.Number ();
	}

	public static int test_2_sealed_override_callee ()
	{
		InliningDerived d = new InliningDerived ();
		return d.Number () - 1;
	}

	// IntPtr methods sit in a class the inliner refuses by index.
	public static int test_8_magic_class_callee ()
	{
		return (int) new IntPtr (Id (8));
	}
}

public struct InliningPair {
	public int First;
	public int Second;
}

public class InliningBase {
	public virtual int Number () { return 1; }
}

public class InliningDerived : InliningBase {
	public sealed override int Number () { return 3; }
}
