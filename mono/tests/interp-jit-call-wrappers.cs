using System;
using System.Collections.Generic;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;

/*
 * Interpreted code calling a wrapper, over the wrapper kinds a call site
 * reaches. Run with --llvm-opt=-mono-tier0-filter=Interpreted so that
 * Interpreted's methods run in the interpreter while everything else
 * compiles: each call Run () makes then leaves the interpreter through
 * do_jit_call ().
 *
 * A dynamic method is the shape this is written for. Tier 0 already ran one,
 * so before this change a compiled dynamic method was still entered by
 * interpreting its bytecode.
 *
 * The same source run without the option is an ordinary test at the default
 * tier, where the wrappers start in the interpreter instead. Both arms have to
 * give the answers below.
 */

struct Pair {
	public int a, b;
}

delegate int Work (int seed);
delegate Pair PairWork (int a, int b);
delegate void Adder (int by);
delegate int Thrower (int a);

class Counter {
	public static int total;

	[MethodImpl (MethodImplOptions.NoInlining | MethodImplOptions.Synchronized)]
	public static int BumpSynchronized (int by)
	{
		total += by;
		return total;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static void Add (int by) { total += by; }
}

class Dynamic {
	/* int loop (int seed) — a body worth compiling, so the jit call pays. */
	public static Work Loop ()
	{
		var m = new DynamicMethod ("loop", typeof (int), new Type [] { typeof (int) },
		                           typeof (Dynamic).Module);
		var il = m.GetILGenerator ();
		var top = il.DefineLabel ();
		var test = il.DefineLabel ();

		il.DeclareLocal (typeof (int));
		il.DeclareLocal (typeof (int));
		il.Emit (OpCodes.Ldarg_0);
		il.Emit (OpCodes.Stloc_0);
		il.Emit (OpCodes.Ldc_I4_0);
		il.Emit (OpCodes.Stloc_1);
		il.Emit (OpCodes.Br, test);

		il.MarkLabel (top);
		il.Emit (OpCodes.Ldloc_0);
		il.Emit (OpCodes.Ldloc_1);
		il.Emit (OpCodes.Add);
		il.Emit (OpCodes.Stloc_0);
		il.Emit (OpCodes.Ldloc_1);
		il.Emit (OpCodes.Ldc_I4_1);
		il.Emit (OpCodes.Add);
		il.Emit (OpCodes.Stloc_1);

		il.MarkLabel (test);
		il.Emit (OpCodes.Ldloc_1);
		il.Emit (OpCodes.Ldc_I4, 10);
		il.Emit (OpCodes.Blt, top);

		il.Emit (OpCodes.Ldloc_0);
		il.Emit (OpCodes.Ret);
		return (Work) m.CreateDelegate (typeof (Work));
	}

	/* A value type returned by value, which the marshalling passes by address. */
	public static PairWork MakePair ()
	{
		var m = new DynamicMethod ("makepair", typeof (Pair),
		                           new Type [] { typeof (int), typeof (int) },
		                           typeof (Dynamic).Module);
		var il = m.GetILGenerator ();

		il.DeclareLocal (typeof (Pair));
		il.Emit (OpCodes.Ldloca_S, (byte) 0);
		il.Emit (OpCodes.Ldarg_0);
		il.Emit (OpCodes.Stfld, typeof (Pair).GetField ("a"));
		il.Emit (OpCodes.Ldloca_S, (byte) 0);
		il.Emit (OpCodes.Ldarg_1);
		il.Emit (OpCodes.Stfld, typeof (Pair).GetField ("b"));
		il.Emit (OpCodes.Ldloc_0);
		il.Emit (OpCodes.Ret);
		return (PairWork) m.CreateDelegate (typeof (PairWork));
	}

	/* Returns void, so the marshalling writes nothing back to the stack. */
	public static Adder Bump ()
	{
		var m = new DynamicMethod ("bump", null, new Type [] { typeof (int) },
		                           typeof (Dynamic).Module);
		var il = m.GetILGenerator ();

		il.Emit (OpCodes.Ldarg_0);
		il.Emit (OpCodes.Call, typeof (Counter).GetMethod ("Add"));
		il.Emit (OpCodes.Ret);
		return (Adder) m.CreateDelegate (typeof (Adder));
	}

	/* Throws, so the exception crosses back over the jit call. */
	public static Thrower Throws ()
	{
		var m = new DynamicMethod ("throws", typeof (int), new Type [] { typeof (int) },
		                           typeof (Dynamic).Module);
		var il = m.GetILGenerator ();

		il.Emit (OpCodes.Ldstr, "thrown");
		il.Emit (OpCodes.Newobj,
		         typeof (InvalidOperationException).GetConstructor (new Type [] { typeof (string) }));
		il.Emit (OpCodes.Throw);
		return (Thrower) m.CreateDelegate (typeof (Thrower));
	}
}

class Interpreted {
	public static int Run (Work loop, PairWork pair, Adder bump, Thrower throws)
	{
		/*
		 * Each callee is called enough times to leave tier 0, so the calls after
		 * the warm-up arrive at a compiled body.
		 */
		for (int i = 0; i < 200; i++) {
			loop (0);
			pair (1, 2);
			bump (0);
			Counter.BumpSynchronized (0);
		}

		Driver.Check ("dynamic loop", loop (5), 50);
		Driver.Check ("dynamic loop again", loop (0), 45);

		Pair made = pair (7, 8);
		Driver.Check ("dynamic pair", made.a * 100 + made.b, 708);

		Counter.total = 0;
		bump (21);
		Driver.Check ("dynamic void", Counter.total, 21);

		/* A synchronized method is a wrapper in front of the body. */
		Counter.total = 0;
		Driver.Check ("synchronized", Counter.BumpSynchronized (4), 4);
		Driver.Check ("synchronized again", Counter.BumpSynchronized (4), 8);

		/*
		 * An array reached through IList<T> goes through the generic array
		 * helper, which is a managed-to-managed wrapper on the array class.
		 */
		int [] numbers = new int [] { 3, 5, 7 };
		IList<int> list = numbers;
		int sum = 0;
		for (int i = 0; i < 100; i++)
			sum = list.Count + list [0];
		Driver.Check ("generic array helper", sum, 6);

		int walked = 0;
		foreach (int n in (IEnumerable<int>) numbers)
			walked += n;
		Driver.Check ("array enumerator", walked, 15);

		/* A multicast delegate is invoked through a delegate-invoke wrapper. */
		Counter.total = 0;
		Adder multi = Counter.Add;
		multi += Counter.Add;
		for (int i = 0; i < 100; i++)
			multi (0);
		multi (3);
		Driver.Check ("multicast delegate", Counter.total, 6);

		/* An exception raised inside the callee, caught here. */
		try {
			throws (1);
			Driver.Check ("throws returned", 1, 0);
		} catch (InvalidOperationException e) {
			Driver.Check ("throws message", e.Message, "thrown");
		}

		/* And one that only this frame's finally sees on the way past. */
		int ran = 0;
		try {
			try {
				throws (2);
			} finally {
				ran = 1;
			}
		} catch (InvalidOperationException) {
		}
		Driver.Check ("finally ran", ran, 1);

		return Driver.failures;
	}
}

class Driver {
	public static int failures;

	public static void Check (string what, long got, long want)
	{
		if (got != want) {
			Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
			failures++;
		}
	}

	public static void Check (string what, string got, string want)
	{
		if (got != want) {
			Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
			failures++;
		}
	}

	public static int Main ()
	{
		Work loop = Dynamic.Loop ();
		PairWork pair = Dynamic.MakePair ();
		Adder bump = Dynamic.Bump ();
		Thrower throws = Dynamic.Throws ();

		return Interpreted.Run (loop, pair, bump, throws) == 0 ? 0 : 1;
	}
}
