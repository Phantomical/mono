using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

// SmallThrower is long enough that the classic JIT's front-end inliner declines
// it (20 IL bytes), short enough that mono's front end would once have folded it
// away for a tier-1 compile before LLVM ever saw the call -- which lost the frame
// outright, since a front-end inline leaves no debug info describing what it
// folded in.  Tier 1 does no front-end inlining now, so this comes back.

class SmallInline {

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

	static int SmallThrower (int x)
	{
		int a = x + 1, b = a * 2, c = b - 3, d = c ^ 5;
		if (d == int.MinValue)
			return -1;
		if (x == 7)
			throw new InvalidOperationException ("small");
		return x + 9;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SmallCaller (int x)
	{
		return SmallThrower (x) + 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Scenario ()
	{
		for (int i = 0; i < 3; i++)
			SmallCaller (i);
		try {
			SmallCaller (7);
		} catch (Exception e) {
			Dump ("small-inline", e);
		}
	}

	public static int Main ()
	{
		Scenario ();
		return 0;
	}
}
