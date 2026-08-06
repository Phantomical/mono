using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.CompilerServices;

// A throw from inside a finally-bearing frame.  The unwinder runs the handler on
// the way past, so the frame is resumed and re-entered before the trace is read
// off it -- the native offset the map is asked about is not the one the throw
// left behind.

class ThrowUnderFinally {

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
	static int Thrower (int x)
	{
		int acc = 0;
		try {
			if (x == 7)
				throw new InvalidOperationException ("under finally");	// IL-FRAME: throw-under-finally 0 ThrowUnderFinally:Thrower
			acc = x + 1;
		} finally {
			acc += 2;
		}
		return acc;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Scenario ()
	{
		for (int i = 0; i < 3; i++)
			Thrower (i);
		try {
			Thrower (7);	// IL-FRAME: throw-under-finally 1 ThrowUnderFinally:Scenario
		} catch (Exception e) {
			Dump ("throw-under-finally", new StackTrace (e, true));
		}
	}

	public static int Main ()
	{
		Scenario ();
		return 0;
	}
}
