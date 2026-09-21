using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Enum.HasFlag () folds to a bit test when both operands are provably the
 * same concrete enum class. Otherwise it falls back to the method itself.
 *
 * Direct () boxes both operands straight off Access-typed parameters, which
 * is the shape the fold reads. ThroughOpaque () passes the receiver through a
 * call declared to return System.Enum. Its class is not settled at the
 * HasFlag () site, so the fold leaves it to the fallback. ByteDirect () repeats
 * the fold case over a byte-backed enum, where a wrong width in the bit test
 * would show up.
 */

[Flags]
enum Access {
	None = 0,
	Read = 1,
	Write = 2,
	Execute = 4,
	ReadWrite = Read | Write,
}

[Flags]
enum ByteAccess : byte {
	None = 0,
	Read = 1,
	Write = 2,
	ReadWrite = Read | Write,
}

class Program {
	const int tier2 = 4;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool Direct (Access value, Access flag)
	{
		return value.HasFlag (flag);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool ByteDirect (ByteAccess value, ByteAccess flag)
	{
		return value.HasFlag (flag);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Enum Opaque (Access value)
	{
		return value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool ThroughOpaque (Access value, Access flag)
	{
		return Opaque (value).HasFlag (flag);
	}

	static bool Check (string name, bool got, bool want)
	{
		if (got == want)
			return true;

		Console.WriteLine ("FAIL: {0} answered {1}, wanted {2}", name, got, want);
		return false;
	}

	static bool RunAll (string tier)
	{
		bool ok = true;

		ok &= Check (tier + " Direct ReadWrite has Read",
		            Direct (Access.ReadWrite, Access.Read), true);
		ok &= Check (tier + " Direct ReadWrite has Execute",
		            Direct (Access.ReadWrite, Access.Execute), false);
		ok &= Check (tier + " Direct None has None", Direct (Access.None, Access.None), true);

		ok &= Check (tier + " ByteDirect ReadWrite has Read",
		            ByteDirect (ByteAccess.ReadWrite, ByteAccess.Read), true);
		ok &= Check (tier + " ByteDirect Read has Write",
		            ByteDirect (ByteAccess.Read, ByteAccess.Write), false);

		ok &= Check (tier + " ThroughOpaque ReadWrite has Read",
		            ThroughOpaque (Access.ReadWrite, Access.Read), true);
		ok &= Check (tier + " ThroughOpaque ReadWrite has Execute",
		            ThroughOpaque (Access.ReadWrite, Access.Execute), false);

		return ok;
	}

	static bool Promote (string name)
	{
		MethodInfo target = typeof (Program).GetMethod (
			name, BindingFlags.Static | BindingFlags.NonPublic);

		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier2))
			return true;

		Console.WriteLine ("FAIL: {0} () would not compile at tier 2", name);
		return false;
	}

	public static int Main ()
	{
		if (!RunAll ("tier 0"))
			return 1;

		if (!Promote ("Direct") || !Promote ("ByteDirect") || !Promote ("Opaque")
		    || !Promote ("ThroughOpaque"))
			return 1;

		if (!RunAll ("tier 2"))
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}
