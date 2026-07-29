using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

// The control: no inlining anywhere, so both traces have the same frames and any
// disagreement is the map itself rather than a recovered inline chain.

class NoInlining {

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

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int OpaqueThrower (int x)
	{
		if (x == 7)
			throw new InvalidOperationException ("opaque");
		return x + 9;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int OpaqueMiddle (int x)
	{
		return OpaqueThrower (x) + 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Scenario ()
	{
		for (int i = 0; i < 3; i++)
			OpaqueMiddle (i);
		try {
			OpaqueMiddle (7);
		} catch (Exception e) {
			Dump ("no-inlining", e);
		}
	}

	public static int Main ()
	{
		Scenario ();
		return 0;
	}
}
