using System;
using System.Runtime.CompilerServices;
using Mono.Overrides;

/*
 * An assembly marked [assembly: MonoOverrideAssembly] has its methods read as
 * it loads, and each one carrying [MonoOverride ("ns.class:method")] replaces
 * the method it names.
 *
 * Every target below is called far more often than the promotion threshold, so
 * each answer is checked against the interpreted body and against the compiled
 * one that replaces it. The replacement has to win in both.
 */

[assembly: MonoOverrideAssembly]

namespace Mono.Test {

	public class Target {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Plain (int x)
		{
			return x + 1;
		}

		/* Small enough for the interpreter to copy into its callers. */
		public static int Small (int x)
		{
			return x + 2;
		}

		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Instance (int x)
		{
			return x + 3;
		}

		/*
		 * Eight parameters, which a native detour cannot carry into an
		 * interpreted caller: mono_interp_jit_call_marshallable () refuses
		 * more than six.
		 */
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Wide (int a, int b, int c, int d, int e, int f, int g, int h)
		{
			return a + b + c + d + e + f + g + h;
		}
	}

	/* A caller of its own, so that the inlined case is reached. */
	public class Caller {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int CallSmall (int x)
		{
			return Target.Small (x);
		}
	}

	public class Overrides {
		[MonoOverride ("Mono.Test.Target:Plain")]
		static int Plain (int x)
		{
			return x + 100;
		}

		[MonoOverride ("Mono.Test.Target:Small")]
		static int Small (int x)
		{
			return x + 200;
		}

		/* Static, with the receiver as the first parameter. */
		[MonoOverride ("Mono.Test.Target:Instance")]
		static int Instance (object self, int x)
		{
			return self != null ? x + 300 : -1;
		}

		[MonoOverride ("Mono.Test.Target:Wide")]
		static int Wide (int a, int b, int c, int d, int e, int f, int g, int h)
		{
			return a + b + c + d + e + f + g + h + 400;
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
			Target instance = new Target ();

			for (int i = 0; i < 200; i++) {
				Check ("plain", i, Target.Plain (i), i + 100);
				Check ("small", i, Target.Small (i), i + 200);
				Check ("small-inlined", i, Caller.CallSmall (i), i + 200);
				Check ("instance", i, instance.Instance (i), i + 300);
				Check ("wide", i, Target.Wide (i, 0, 0, 0, 0, 0, 0, 0), i + 400);
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
