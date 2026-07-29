using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

// Two inlinable bodies stacked, so that once tier 1 folds both into Level1 a
// single native offset stands for three methods.  A chain the runtime reports in
// the wrong order, or truncates to its innermost body, still looks right with
// only one level to get wrong.

class NestedInline {

	static void Dump (string label, Exception e)
	{
		StackTrace st = new StackTrace (e, false);
		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			var m = f.GetMethod ();
			if (m == null)
				continue;
			Console.WriteLine ("{0}\t{1}:{2}\t0x{3:x}", label, m.DeclaringType.Name, m.Name, f.GetILOffset ());
		}
	}

	static int Level3 (int x)
	{
		int p1 = x + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
		int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		if (x == 7)
			throw new InvalidOperationException ("level3");
		return x + 9;
	}

	/*
	 * Padded on a constant rather than on the argument.  The two inliners measure
	 * different things: mono's goes by raw IL length, so this stays out of its
	 * reach, while LLVM's cost model folds the whole run to a constant and charges
	 * nothing for it.  That is what leaves LLVM room to take Level2 *and* the
	 * Level3 already folded into it, making the chain two deep rather than one.
	 */
	static int Level2 (int x)
	{
		int c = 1;
		c += 2; c += 3; c += 4; c += 5; c += 6; c += 7; c += 8; c += 9;
		c += 10; c += 11; c += 12; c += 13; c += 14; c += 15; c += 16;
		c += 17; c += 18; c += 19; c += 20; c += 21; c += 22; c += 23;
		c += 24; c += 25; c += 26; c += 27; c += 28; c += 29; c += 30;
		if (c == 0)
			return -1;
		return Level3 (x);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Level1 (int x)
	{
		return Level2 (x) + 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Scenario ()
	{
		for (int i = 0; i < 3; i++)
			Level1 (i);
		try {
			Level1 (7);
		} catch (Exception e) {
			Dump ("nested-inline", e);
		}
	}

	public static int Main ()
	{
		Scenario ();
		return 0;
	}
}
