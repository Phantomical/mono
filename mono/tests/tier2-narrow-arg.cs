using System;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * A narrow integer that crosses a dispatched call site.
 *
 * The caller fills the high bits of a char argument and the callee reads them,
 * so the two have to agree on which way they are filled. The agreement is an
 * attribute on the call site, and a site that dispatches through a slot names no
 * callee to take it from. Lookup () then reads a register the caller never
 * finished writing, and answers null for a symbol it knows.
 *
 * The shape matters more than the loop. The char arrives from
 * IEnumerator<char>.get_Current (), which leaves the high half of its return
 * register alone. It leaves for ISymbols.Lookup (char), which at tier 2 compares
 * the whole register. Both ends are dispatched calls, and the tiers have to
 * reach both before the two disagree.
 *
 * A run where the high bits are zero by luck passes whatever the site says, so
 * this test can only fail when the defect is there.
 */

interface ISymbols {
	object Lookup (char symbol);

	object Lookup (string symbol);
}

class Symbols : ISymbols {
	static readonly object a = new object ();
	static readonly object c = new object ();
	static readonly object g = new object ();
	static readonly object t = new object ();

	public object Lookup (char symbol)
	{
		switch (symbol) {
		case 'A':
			return a;
		case 'C':
			return c;
		case 'G':
			return g;
		case 'T':
			return t;
		default:
			return null;
		}
	}

	public object Lookup (string symbol)
	{
		return Lookup (symbol[0]);
	}
}

static class Program {
	/* MonoTier::tier2, as PromoteNow takes it. */
	const int tier2 = 3;

	/* Calls of Accepts (). */
	const int calls = 200000;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool Accepts (ISymbols symbols, string text)
	{
		foreach (char symbol in text.Distinct ())
			if (symbols.Lookup (symbol) == null)
				return false;

		return true;
	}

	/// Puts one of this program's own methods at tier 2, and says so when the
	/// backend declines.
	static bool Promote (Type type, string name)
	{
		MethodInfo target = type.GetMethod (name,
			BindingFlags.Static | BindingFlags.Instance
				| BindingFlags.Public | BindingFlags.NonPublic,
			null, new Type[] { typeof (char) }, null);

		if (target == null)
			target = type.GetMethod (name,
				BindingFlags.Static | BindingFlags.NonPublic);

		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier2))
			return true;

		Console.WriteLine ("FAIL: {0} () would not compile at tier 2", name);
		return false;
	}

	public static int Main ()
	{
		ISymbols symbols = new Symbols ();

		if (!Promote (typeof (Program), "Accepts") || !Promote (typeof (Symbols), "Lookup"))
			return 1;

		for (int i = 0; i < calls; ++i) {
			if (Accepts (symbols, "C"))
				continue;

			Console.WriteLine ("FAIL: call {0} refused a symbol the alphabet has", i);
			Console.WriteLine ("      a direct lookup of it answers {0}",
			                   symbols.Lookup ('C') == null ? "null" : "the symbol");
			return 1;
		}

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
