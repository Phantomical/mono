using System;
using System.Reflection;
using System.Runtime.CompilerServices;

// Verify that ldftn and ldvirtftn reuse the backend thunk for static rgctx
// methods across tier changes.

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

delegate int NoArgs ();

interface INamed {
	int Size ();
}

struct Cell<T> : INamed {
	public T a, b;

	public int Size () { return (a == null ? 0 : 1) + (b == null ? 0 : 1) + 40; }
}

public class FuncptrStaticRgctx {
	static int fails;

	static void Check (string what, int got, int want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
		++fails;
	}

	// A direct delegate over the struct's instance method.
	static void RoundDirect (string tier)
	{
		Cell<string> cell = new Cell<string> { a = "x", b = "y" };
		NoArgs d = cell.Size;

		Check ("direct at " + tier, d (), 42);
	}

	// A delegate over the interface method on a boxed receiver.
	static void RoundUnbox (string tier)
	{
		object boxed = new Cell<string> { a = "x", b = "y" };
		MethodInfo im = typeof (INamed).GetMethod ("Size");
		NoArgs d = (NoArgs) Delegate.CreateDelegate (typeof (NoArgs), boxed, im);

		Check ("unbox at " + tier, d (), 42);
	}

	static bool Promote (int tier, string name)
	{
		MethodInfo target = typeof (Cell<string>).GetMethod ("Size");

		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: Cell<string>.Size would not compile at {0}", name);
		++fails;
		return false;
	}

	public static int Main ()
	{
		RoundDirect ("tier 0");
		RoundUnbox ("tier 0");

		if (!Promote (3, "tier 1"))
			return 1;

		RoundDirect ("tier 1");
		RoundUnbox ("tier 1");

		if (!Promote (4, "tier 2"))
			return 1;

		RoundDirect ("tier 2");
		RoundUnbox ("tier 2");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
