using System;
using System.Runtime.CompilerServices;
using Mono.Overrides;

/*
 * Generic targets. An override is written against the target's definition and
 * is instantiated with each of the target's own type arguments, so one
 * attribute covers every instantiation - including the ones that do not exist
 * when the assembly is read.
 *
 * A native detour reaches none of this from an interpreted caller:
 * mono_interp_jit_call_marshallable () refuses an inflated method outright.
 */

[assembly: MonoOverrideAssembly]

namespace Mono.Test {

	public class Target {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Method<T> (int x)
		{
			return x + 1;
		}
	}

	/* A generic class, whose type argument the override has to receive too. */
	public class Holder<T> {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Read (int x)
		{
			return x + 2;
		}

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Both<U> (int x)
		{
			return x + 3;
		}
	}

	public class Overrides {
		[MonoOverride ("Mono.Test.Target:Method")]
		static int Method<T> (int x)
		{
			return x + 100;
		}

		/*
		 * The class's type argument arrives as the override's own, since the
		 * override is a static method of a class that is not generic.
		 */
		[MonoOverride ("Mono.Test.Holder`1:Read")]
		static int Read<T> (int x)
		{
			return x + 200;
		}

		/* The class's argument first, then the method's. */
		[MonoOverride ("Mono.Test.Holder`1:Both")]
		static int Both<T, U> (int x)
		{
			return x + 300;
		}
	}

	public class Test {
		static int failures;

		static void Check (string what, int round, long got, long want)
		{
			if (got == want)
				return;

			Console.WriteLine ("{0} round {1}: got {2}, want {3}", what, round, got,
			                   want);
			failures++;
		}

		public static int Main ()
		{
			for (int i = 0; i < 200; i++) {
				Check ("method-int", i, Target.Method<int> (i), i + 100);
				Check ("method-string", i, Target.Method<string> (i), i + 100);

				Check ("class-int", i, Holder<int>.Read (i), i + 200);
				Check ("class-string", i, Holder<string>.Read (i), i + 200);

				Check ("both-int", i, Holder<int>.Both<int> (i), i + 300);
				Check ("both-string", i, Holder<string>.Both<object> (i), i + 300);
			}

			if (failures != 0) {
				Console.WriteLine ("{0} failures", failures);
				return 1;
			}

			Console.WriteLine ("OK");
			return 0;
		}
	}
}
