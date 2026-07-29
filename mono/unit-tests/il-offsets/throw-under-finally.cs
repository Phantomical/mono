using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

// A throw from inside a finally-bearing frame, which unwinds differently.

class ThrowUnderFinally {

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
	static int Thrower (int x)
	{
		int acc = 0;
		try {
			if (x == 7)
				throw new InvalidOperationException ("under finally");
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
			Thrower (7);
		} catch (Exception e) {
			Dump ("throw-under-finally", e);
		}
	}

	public static int Main ()
	{
		Scenario ();
		return 0;
	}
}
