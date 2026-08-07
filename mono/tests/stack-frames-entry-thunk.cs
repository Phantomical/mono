using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

// A method reached through its entry thunk - anything the runtime invokes by
// reflection - must appear once in a stack trace, not twice. The thunk only
// exists to shuffle arguments into the body's convention, and for most
// signatures it tail-calls the body and leaves no frame at all. The shapes
// below are the ones where it cannot: a value type return and a value type
// argument passed in memory both make it copy through a slot of its own.

public struct Big { public long a, b, c, d, e, f, g, h; }
public struct Pair { public int a, b; }

public class Tests {

	static int failures;

	static void Check (string what, int got, int want)
	{
		if (got != want) {
			Console.WriteLine ("FAIL: {0}: got {1} frames, expected {2}", what, got, want);
			failures++;
		}
	}

	static void CheckPresent (string what, int got)
	{
		if (got < 1) {
			Console.WriteLine ("FAIL: {0}: no frame", what);
			failures++;
		}
	}

	static int CountLive (string name)
	{
		StackTrace st = new StackTrace ();
		int n = 0;

		for (int i = 0; i < st.FrameCount; i++) {
			MethodBase m = st.GetFrame (i).GetMethod ();

			if (m != null && m.Name == name)
				n++;
		}

		return n;
	}

	static int CountInTrace (string trace, string name)
	{
		int n = 0, at = 0;

		while ((at = trace.IndexOf (name, at, StringComparison.Ordinal)) >= 0) {
			n++;
			at += name.Length;
		}

		return n;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Big RetBig (int x)
	{
		Check ("RetBig live", CountLive ("RetBig"), 1);
		throw new InvalidOperationException ("RetBig");
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Pair RetPair (int x)
	{
		Check ("RetPair live", CountLive ("RetPair"), 1);
		throw new InvalidOperationException ("RetPair");
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int TakeBig (Big b)
	{
		Check ("TakeBig live", CountLive ("TakeBig"), 1);
		throw new InvalidOperationException ("TakeBig");
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	public static int RetInt (int x)
	{
		Check ("RetInt live", CountLive ("RetInt"), 1);
		throw new InvalidOperationException ("RetInt");
	}

	// The entry thunk of an instance method on a value type gets a second one,
	// for callers holding the boxed receiver.
	public struct Holder {
		public long a, b, c, d, e, f, g, h;

		[MethodImpl (MethodImplOptions.NoInlining)]
		public Big Unboxed (int x)
		{
			Check ("Unboxed live", CountLive ("Unboxed"), 1);
			throw new InvalidOperationException ("Unboxed");
		}
	}

	// A filter body is a function of its own too, but unlike an entry thunk it
	// is a stretch of the method's IL, so it keeps a frame - and a walk that
	// reaches it has to be able to carry on past it.
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Big Filtered (int x)
	{
		try {
			throw new ArgumentException ("inner");
		} catch (Exception) when (InsideFilter ()) {
			return new Big ();
		}
	}

	static int filter_frames = -1;
	static int filter_reached_main = -1;

	static bool InsideFilter ()
	{
		filter_frames = CountLive ("Filtered");
		filter_reached_main = CountLive ("Main");
		return true;
	}

	static void Invoke (string name, object target, object [] args)
	{
		MethodInfo mi = target == null
			? typeof (Tests).GetMethod (name)
			: target.GetType ().GetMethod (name);

		try {
			mi.Invoke (target, args);
		} catch (TargetInvocationException tie) {
			Check (name + " thrown", CountInTrace (tie.InnerException.StackTrace, name), 1);
		}
	}

	public static int Main ()
	{
		Invoke ("RetBig", null, new object [] { 3 });
		Invoke ("RetPair", null, new object [] { 3 });
		Invoke ("TakeBig", null, new object [] { new Big () });
		Invoke ("RetInt", null, new object [] { 3 });
		Invoke ("Unboxed", new Holder (), new object [] { 3 });

		// How many frames a filter body gets is the execution engine's business
		// - the interpreter's model is not the JIT's - but it is a frame, and a
		// walk that reaches it has to get past it to the frames underneath.
		typeof (Tests).GetMethod ("Filtered").Invoke (null, new object [] { 3 });
		CheckPresent ("Filtered filter", filter_frames);
		Check ("Filtered filter walks on", filter_reached_main, 1);

		return failures;
	}
}
