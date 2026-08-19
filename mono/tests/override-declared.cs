using System;
using System.Runtime.CompilerServices;
using Mine = Elsewhere.Overrides;

/*
 * Both attributes declared here rather than taken from corlib. The runtime
 * matches them by name and by namespace, never by type identity, which is what
 * lets an assembly built against another mscorlib carry overrides - a mod
 * compiled against a stock BCL has no way to name this tree's corlib types.
 *
 * Everything else in this directory uses corlib's copies, so this is the only
 * place the by-name path is checked.
 */

[assembly: Mono.Overrides.MonoOverrideAssembly]

namespace Mono.Overrides {

	[AttributeUsage (AttributeTargets.Assembly)]
	public sealed class MonoOverrideAssemblyAttribute : Attribute {
	}

	[AttributeUsage (AttributeTargets.Method)]
	public sealed class MonoOverrideAttribute : Attribute {
		public MonoOverrideAttribute (string target)
		{
			Target = target;
		}

		public string Target { get; private set; }
	}
}

namespace Elsewhere.Overrides {

	/*
	 * The same two names under a namespace of its own. Neither is the
	 * attribute the runtime looks for, so a match on the name alone would
	 * take these and replace Ignored below.
	 */
	[AttributeUsage (AttributeTargets.Assembly)]
	public sealed class MonoOverrideAssemblyAttribute : Attribute {
	}

	[AttributeUsage (AttributeTargets.Method)]
	public sealed class MonoOverrideAttribute : Attribute {
		public MonoOverrideAttribute (string target)
		{
			Target = target;
		}

		public string Target { get; private set; }
	}
}

namespace Mono.Test {

	public class Target {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Value (int x)
		{
			return x + 1;
		}

		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Ignored (int x)
		{
			return x + 2;
		}
	}

	public class Overrides {
		[Mono.Overrides.MonoOverride ("Mono.Test.Target:Value")]
		static int Value (int x)
		{
			return x + 100;
		}

		/* Right name, wrong namespace, so the runtime leaves the target alone. */
		[Mine.MonoOverride ("Mono.Test.Target:Ignored")]
		static int Ignored (int x)
		{
			return x + 200;
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
				Check ("declared", i, Target.Value (i), i + 100);
				Check ("other-namespace", i, Target.Ignored (i), i + 2);
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
