using System;
using System.Reflection;
using System.Runtime.CompilerServices;

// Exercise class-init guarding when the vtable comes from a shared rgctx fetch.

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class Log {
	public static int Runs;
}

class Gen<T> {
	public static int Value;

	static Gen ()
	{
		Log.Runs++;
		Value = 42;
	}
}

static class Reader<T> where T : class {
	public static int Read ()
	{
		return Gen<T>.Value;
	}
}

class CctorInitGshared {
	const int tier2 = 4;

	static int fails;

	static void Fail (string message)
	{
		Console.WriteLine ("FAIL: {0}", message);
		++fails;
	}

	static int Main ()
	{
		// Both instantiations use the shared compiled body.
		MethodInfo read = typeof (Reader<object>).GetMethod ("Read");

		if (!Mono.Tiering.MonoTier.PromoteNow (read.MethodHandle.Value, tier2))
			Fail ("Reader<T>.Read () would not compile at tier 2");

		int a = Reader<object>.Read ();
		int b = Reader<string>.Read ();
		int c = Reader<object>.Read ();

		if (a != 42 || b != 42 || c != 42)
			Fail (String.Format ("Reader<T>.Read () returned {0}/{1}/{2}, expected 42/42/42",
			                     a, b, c));

		// Each closed Gen<T> has its own class constructor.
		if (Log.Runs != 2)
			Fail (String.Format ("Gen<T>'s cctor ran {0} times total, expected 2", Log.Runs));

		Console.WriteLine (fails == 0 ? "OK" : String.Format ("{0} failure(s)", fails));
		return fails == 0 ? 0 : 1;
	}
}
