using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

// Several throws differing only in a constant argument.  LLVM's cross-jumping
// will happily fold these into one shared call once the argument is hoisted, at
// which point a nearest-preceding-marker lookup can only recover whichever site
// survived -- so this is the fixture for that staying blocked.

class MergeableThrows {

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
	static int MultiThrow (int x)
	{
		if (x == 1)
			throw new InvalidOperationException ("one");
		if (x == 2)
			throw new InvalidOperationException ("two");
		if (x == 3)
			throw new InvalidOperationException ("three");
		return x;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Scenario ()
	{
		for (int i = 4; i < 8; i++)
			MultiThrow (i);
		for (int want = 1; want <= 3; want++) {
			try {
				MultiThrow (want);
			} catch (Exception e) {
				Dump ("mergeable-throw-" + want, e);
			}
		}
	}

	public static int Main ()
	{
		Scenario ();
		return 0;
	}
}
