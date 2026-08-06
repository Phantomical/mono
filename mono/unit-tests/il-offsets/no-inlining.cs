using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;

// The control: a plain call chain with nothing in it for the optimizer to move,
// so a frame blamed on the wrong line here is the map itself rather than
// something the pipeline did to the body.

class NoInlining {

	static void Dump (string label, StackTrace st)
	{
		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			var m = f.GetMethod ();
			if (m == null)
				continue;
			Console.WriteLine ("{0}\t{1}:{2}\t{3}:{4}", label, m.DeclaringType.Name, m.Name,
					   Path.GetFileName (f.GetFileName ()), f.GetFileLineNumber ());
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int OpaqueThrower (int x)
	{
		if (x == 7)
			throw new InvalidOperationException ("opaque");	// IL-FRAME: no-inlining 0 NoInlining:OpaqueThrower
		return x + 9;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int OpaqueMiddle (int x)
	{
		return OpaqueThrower (x) + 1;	// IL-FRAME: no-inlining 1 NoInlining:OpaqueMiddle
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Scenario ()
	{
		for (int i = 0; i < 3; i++)
			OpaqueMiddle (i);
		try {
			OpaqueMiddle (7);	// IL-FRAME: no-inlining 2 NoInlining:Scenario
		} catch (Exception e) {
			Dump ("no-inlining", new StackTrace (e, true));
		}
	}

	public static int Main ()
	{
		Scenario ();
		return 0;
	}
}
