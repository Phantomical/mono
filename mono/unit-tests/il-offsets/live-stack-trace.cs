using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

// The same question for a trace taken off the running stack rather than out of an
// exception.  It reaches the runtime through a different icall
// (ves_icall_get_frame_info, one frame per call, driven by a skip count) than the
// exception scenarios, so an inlined body can be missing from one and present in
// the other.

class LiveStackTrace {

	/*
	 * The IL offset is printed as "-" rather than compared.  For a live frame the
	 * classic JIT resolves an offset only through the symbol file, so with none
	 * present it reports 0 for every frame, while a tier-1 frame answers from its
	 * own map -- the two disagree for reasons that have nothing to do with
	 * inlining.  Which methods appear, and in what order, is what this is about.
	 */
	static void Dump (string label, StackTrace st)
	{
		for (int i = 0; i < st.FrameCount; i++) {
			var m = st.GetFrame (i).GetMethod ();
			if (m == null)
				continue;
			Console.WriteLine ("{0}\t{1}:{2}\t-", label, m.DeclaringType.Name, m.Name);
			if (m.Name == "Scenario")
				break;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void LiveLeaf ()
	{
		Dump ("live-inlined", new StackTrace (false));
	}

	/*
	 * The body that gets inlined has to sit between the capture and the frame we
	 * want it under, not do the capturing itself - constructing a StackTrace costs
	 * more than the inliner will spend, so a capturing method never gets folded in
	 * and the scenario would prove nothing.  Padded on a constant, as
	 * nested-inline.cs explains, so that only LLVM's inliner takes it.
	 */
	static void LiveMiddle (int x)
	{
		int c = 1;
		c += 2; c += 3; c += 4; c += 5; c += 6; c += 7; c += 8; c += 9;
		c += 10; c += 11; c += 12; c += 13; c += 14; c += 15; c += 16;
		c += 17; c += 18; c += 19; c += 20; c += 21; c += 22; c += 23;
		c += 24; c += 25; c += 26; c += 27; c += 28; c += 29; c += 30;
		if (c == 0)
			return;
		if (x == 7)
			LiveLeaf ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void LiveCaller (int x)
	{
		LiveMiddle (x);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Scenario ()
	{
		for (int i = 0; i < 3; i++)
			LiveCaller (i);
		LiveCaller (7);
	}

	public static int Main ()
	{
		Scenario ();
		return 0;
	}
}
