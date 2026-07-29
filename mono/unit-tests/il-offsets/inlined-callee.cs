using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

// Tier 1 may fold InlinableThrower into InlinableCaller, which drops its frame
// from the trace.  The offset InlinableCaller then reports has to stay its own
// call site rather than become an offset into the callee.

class InlinedCallee {

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

	static int InlinableThrower (int x)
	{
		int p1 = x + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
		int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		if (x == 7)
			throw new InvalidOperationException ("inlinable");
		return x + 9;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int InlinableCaller (int x)
	{
		int a = InlinableThrower (x);
		return a + 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Scenario ()
	{
		for (int i = 0; i < 3; i++)
			InlinableCaller (i);
		try {
			InlinableCaller (7);
		} catch (Exception e) {
			Dump ("inlined-callee", e);
		}
	}

	public static int Main ()
	{
		Scenario ();
		return 0;
	}
}
