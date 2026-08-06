using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;

// Three throws differing only in a constant argument.  SimplifyCFG will happily
// cross-jump these into one shared call once the argument is hoisted, at which
// point all three land on the same native address and a map built by
// nearest-preceding-marker can only recover whichever site survived.  Each of the
// three traces below has to come back naming its own throw.

class MergeableThrows {

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
	static int MultiThrow (int x)
	{
		if (x == 1)
			throw new InvalidOperationException ("one");	// IL-FRAME: mergeable-throw-1 0 MergeableThrows:MultiThrow
		if (x == 2)
			throw new InvalidOperationException ("two");	// IL-FRAME: mergeable-throw-2 0 MergeableThrows:MultiThrow
		if (x == 3)
			throw new InvalidOperationException ("three");	// IL-FRAME: mergeable-throw-3 0 MergeableThrows:MultiThrow
		return x;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Scenario ()
	{
		for (int i = 4; i < 8; i++)
			MultiThrow (i);
		for (int want = 1; want <= 3; want++) {
			try {
				MultiThrow (want);	// IL-FRAME: mergeable-throw-1,mergeable-throw-2,mergeable-throw-3 1 MergeableThrows:Scenario
			} catch (Exception e) {
				Dump ("mergeable-throw-" + want, new StackTrace (e, true));
			}
		}
	}

	public static int Main ()
	{
		Scenario ();
		return 0;
	}
}
