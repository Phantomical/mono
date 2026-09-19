using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Model standalone System.Numerics.Vectors assemblies with private copies of
 * System.Numerics.Vector. The intrinsic getter must return true before and
 * after tier 2 promotion; the unmarked control must retain its false result.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Program {
	const int Tier2 = 4;

	static bool ok = true;

	static MethodInfo Getter (string dll)
	{
		string path = Path.Combine (AppDomain.CurrentDomain.BaseDirectory, dll);
		Assembly lib = Assembly.LoadFrom (path);
		Type vector = lib.GetType ("System.Numerics.Vector");

		return vector.GetMethod ("get_IsHardwareAccelerated");
	}

	static void Check (MethodInfo getter, bool want, string when)
	{
		bool got = (bool) getter.Invoke (null, null);

		if (got == want)
			return;

		Console.WriteLine ("FAIL: {0} answered {1}, wanted {2}", when, got, want);
		ok = false;
	}

	static void Run (string dll, bool want)
	{
		MethodInfo getter = Getter (dll);

		Check (getter, want, dll + " at first call");

		if (!Mono.Tiering.MonoTier.PromoteNow (getter.MethodHandle.Value, Tier2)) {
			Console.WriteLine ("FAIL: {0}'s getter would not promote to tier 2", dll);
			ok = false;
			return;
		}

		Check (getter, want, dll + " at tier 2");
	}

	public static int Main ()
	{
		Run ("vector-hwaccel-lib.dll", true);
		Run ("vector-hwaccel-unmarked-lib.dll", false);

		return ok ? 0 : 1;
	}
}
