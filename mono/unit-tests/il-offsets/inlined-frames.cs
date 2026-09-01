using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;

// A body an inliner folded in still gets a frame of its own, and that frame is
// blamed on the line inside the folded body rather than on the call site it was
// folded through. The caller keeps its frame, on its own line, so one stretch of
// code has to come back as several frames blamed on several lines.
//
// The nested scenario folds a body into a body that was itself folded in, which
// is what puts more than one frame on a single call site and orders them.
//
// Both compiled tiers fold, so each scenario is run at each, and the two have to
// answer the same. Mono.Tiering.MonoTier::PromoteNow compiles a method at the
// tier it is given, on this thread, so the fixture needs no environment and
// races no compile worker.

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class InlinedFrames {

	/*
	 * Stop unless the frame reported for `inlined` covers `host`'s code.
	 *
	 * The lines the markers check are the same whether a helper was folded in or
	 * called, so on their own they would pass against a run that folded nothing.
	 * A folded body owns no code: its frame reports the offset into the body it
	 * was folded into, which is what tells the two apart. Running with
	 * --llvm-opt=-mono-inline-il-limit=0 is what this refuses.
	 */
	static void MustBeFolded (StackTrace st, string inlined, string host)
	{
		int in_helper = -1, in_host = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			var m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.Name == inlined)
				in_helper = f.GetNativeOffset ();
			if (m.Name == host)
				in_host = f.GetNativeOffset ();
		}

		if (in_helper >= 0 && in_helper == in_host)
			return;

		Console.Error.WriteLine ("{0} was not folded into {1}: offsets {2} and {3}",
					 inlined, host, in_helper, in_host);
		Environment.Exit (1);
	}

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

	// A straight line to one call and then a throw, which is the shape the
	// pre-pass in front of both tiers folds.
	static int Thrower (int x)
	{
		throw new InvalidOperationException ("folded");	// IL-FRAME: flat1,flat2 0 InlinedFrames:Thrower
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Flat (int x)
	{
		return Thrower (x) + 1;	// IL-FRAME: flat1,flat2 1 InlinedFrames:Flat
	}

	static int Inner (int x)
	{
		throw new InvalidOperationException ("deep");	// IL-FRAME: nested1,nested2 0 InlinedFrames:Inner
	}

	// A plain forwarder - the shape test takes nothing between the call and the
	// return - so the fold that takes this one takes Inner () with it.
	static int Outer (int x)
	{
		return Inner (x);	// IL-FRAME: nested1,nested2 1 InlinedFrames:Outer
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Nested (int x)
	{
		return Outer (x) + 1;	// IL-FRAME: nested1,nested2 2 InlinedFrames:Nested
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Scenario (string tier)
	{
		try {
			Flat (7);	// IL-FRAME: flat1,flat2 2 InlinedFrames:Scenario
		} catch (Exception e) {
			StackTrace st = new StackTrace (e, true);

			MustBeFolded (st, "Thrower", "Flat");
			Dump ("flat" + tier, st);
		}

		try {
			Nested (7);	// IL-FRAME: nested1,nested2 3 InlinedFrames:Scenario
		} catch (Exception e) {
			StackTrace st = new StackTrace (e, true);

			MustBeFolded (st, "Inner", "Nested");
			MustBeFolded (st, "Outer", "Nested");
			Dump ("nested" + tier, st);
		}
	}

	/* MonoTier::tier1 and MonoTier::tier2, as PromoteNow takes them. */
	const int tier1 = 2;
	const int tier2 = 3;

	static void AtTier (int tier, string label)
	{
		Type self = typeof (InlinedFrames);

		foreach (string name in new [] { "Scenario", "Flat", "Nested" }) {
			MethodInfo m = self.GetMethod (name, BindingFlags.Static | BindingFlags.NonPublic);

			if (!Mono.Tiering.MonoTier.PromoteNow (m.MethodHandle.Value, tier)) {
				Console.Error.WriteLine ("{0} would not compile at tier {1}", name, label);
				Environment.Exit (1);
			}
		}

		Scenario (label);
	}

	public static int Main ()
	{
		AtTier (tier1, "1");
		AtTier (tier2, "2");
		return 0;
	}
}
