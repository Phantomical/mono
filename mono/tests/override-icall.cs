using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Mono.Overrides.MonoOverride::Install, which replaces a method the attribute
 * cannot name. Harmony is handed a MethodBase at run time and has no name to
 * write down, so this is the call its shim makes.
 *
 * It also covers what an attribute cannot reach: a method replaced twice, and
 * the address a caller already holds still being the one that reaches the
 * replacement. Harmony re-points a method on every patch and on every unpatch
 * alike, so a method three mods patch is re-pointed five or six times.
 */

namespace Mono.Overrides {

	/* The runtime registers this as it starts, whatever declares it. */
	public static class MonoOverride {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern void Install (IntPtr target, IntPtr replacement);
	}
}

namespace Mono.Test {

	public class Target {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Replaced (int x)
		{
			return x + 1;
		}

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Retargeted (int x)
		{
			return x + 2;
		}
	}

	public class Replacements {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int First (int x)
		{
			return x + 100;
		}

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Second (int x)
		{
			return x + 200;
		}
	}

	public class Test {
		static int failures;

		static void Check (string what, long got, long want)
		{
			if (got == want)
				return;

			Console.WriteLine ("{0}: got {1}, want {2}", what, got, want);
			failures++;
		}

		static MethodInfo MethodOf (Type type, string name)
		{
			return type.GetMethod (name, BindingFlags.Public | BindingFlags.Static);
		}

		static void Install (MethodInfo target, MethodInfo replacement)
		{
			Mono.Overrides.MonoOverride.Install (target.MethodHandle.Value,
			                                     replacement.MethodHandle.Value);
		}

		public static int Main ()
		{
			MethodInfo replaced = MethodOf (typeof (Target), "Replaced");
			MethodInfo retargeted = MethodOf (typeof (Target), "Retargeted");
			MethodInfo first = MethodOf (typeof (Replacements), "First");
			MethodInfo second = MethodOf (typeof (Replacements), "Second");

			/* Warm both, so that the entry is taken from a compiled method. */
			for (int i = 0; i < 200; i++) {
				Target.Replaced (i);
				Target.Retargeted (i);
			}

			IntPtr before = retargeted.MethodHandle.GetFunctionPointer ();

			Install (replaced, first);
			Install (retargeted, first);

			for (int i = 0; i < 200; i++) {
				Check ("replaced", Target.Replaced (i), i + 100);
				Check ("retargeted-first", Target.Retargeted (i), i + 100);
			}

			/* A second override replaces the first rather than being refused. */
			Install (retargeted, second);

			for (int i = 0; i < 200; i++)
				Check ("retargeted-second", Target.Retargeted (i), i + 200);

			/*
			 * The entry is where a caller already holding the address arrives,
			 * so it has to be the same address rather than a new one.
			 */
			IntPtr after = retargeted.MethodHandle.GetFunctionPointer ();

			if (before != after) {
				Console.WriteLine ("entry moved: {0} then {1}", before, after);
				failures++;
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
