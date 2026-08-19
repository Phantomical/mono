using System;
using System.Runtime.CompilerServices;
using Mono.Overrides;

/*
 * Built twice, under two assembly names, for override-copies.cs. A target is
 * matched by name in every loaded image rather than by type identity, because
 * a process full of Harmony repacks has several copies of one assembly loaded
 * and every copy has to be replaced.
 *
 * Each copy carries the override of its own target, so a copy that is never
 * read answers the original value and says so.
 */

[assembly: MonoOverrideAssembly]

namespace Mono.Test {

	public class Copied {
		[MethodImpl (MethodImplOptions.NoInlining)]
		public static int Value (int x)
		{
			return x + 1;
		}
	}

	public class Overrides {
		[MonoOverride ("Mono.Test.Copied:Value")]
		static int Value (int x)
		{
			return x + 100;
		}
	}
}
