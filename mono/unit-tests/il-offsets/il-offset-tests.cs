using System;
using System.Diagnostics;
using System.Runtime.CompilerServices;

/*
 * Fixtures for check-il-offsets.sh: each scenario throws, catches in its own
 * entry point, and prints one line per managed frame with the frame's IL offset.
 * The script runs this under the classic JIT and under tier 1 and compares.
 *
 * Nothing here asserts an offset by value - the classic JIT is the oracle, so a
 * fixture only has to produce a trace that is interesting to compare. What makes
 * one interesting is a frame whose IL offset could plausibly be attributed to the
 * wrong method: a call site that tier 1 may inline through, a throw the optimizer
 * may move or merge, or a frame below both.
 *
 * Each catch is in the scenario entry point, so Main never appears in a trace and
 * the compared frames are exactly the ones the fixture is about.
 */
class ILOffsetTests {

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

	/*
	 * Frames taken off the live stack, stopping at the scenario entry point so that
	 * Main and the runtime's invoke wrappers stay out of the comparison.
	 *
	 * The IL offset is printed as "-" rather than compared. For a live frame the
	 * classic JIT resolves an offset only through the symbol file, so with none
	 * present it reports 0 for every frame, while a tier-1 frame answers from its
	 * own map - the two disagree for reasons that have nothing to do with inlining.
	 * Which methods appear, and in what order, is what this scenario is about; the
	 * exception scenarios are where offsets get checked.
	 */
	static void DumpLive (string label, StackTrace st)
	{
		for (int i = 0; i < st.FrameCount; i++) {
			var m = st.GetFrame (i).GetMethod ();
			if (m == null)
				continue;
			Console.WriteLine ("{0}\t{1}:{2}\t-", label, m.DeclaringType.Name, m.Name);
			if (m.Name == "ScenarioLiveStackTrace")
				break;
		}
	}

	/*
	 * Padded to ~15 statements for the same reason the inliner fixtures are: without
	 * it the classic JIT folds the call away before tier 1 ever sees it, and the
	 * scenario stops being about tier-1 inlining.
	 */
	static int InlinableThrower (int x)
	{
		int p1 = x + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
		int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		if (x == 7)
			throw new InvalidOperationException ("inlinable");
		return x + 9;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int InlinableCaller (int x)
	{
		int a = InlinableThrower (x);
		return a + 1;
	}

	/*
	 * The frame that matters: tier 1 may fold InlinableThrower into
	 * InlinableCaller, which drops its frame from the trace. The IL offset
	 * InlinableCaller then reports has to stay its own call site rather than
	 * become an offset into the callee.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ScenarioInlinedCallee ()
	{
		for (int i = 0; i < 3; i++)
			InlinableCaller (i);
		try {
			InlinableCaller (7);
		} catch (Exception e) {
			Dump ("inlined-callee", e);
		}
	}

	/*
	 * Long enough that the classic JIT's front-end inliner declines it (20 IL
	 * bytes), short enough that mono's front end would once have folded it away
	 * for a tier-1 compile before LLVM ever saw the call - which lost the frame
	 * outright, since a front-end inline leaves no debug info describing what it
	 * folded in. Tier 1 does no front-end inlining now, so this comes back.
	 */
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
	static void ScenarioSmallInline ()
	{
		for (int i = 0; i < 3; i++)
			SmallCaller (i);
		try {
			SmallCaller (7);
		} catch (Exception e) {
			Dump ("small-inline", e);
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

	/* No inlining anywhere, so both traces have the same frames. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ScenarioNoInlining ()
	{
		for (int i = 0; i < 3; i++)
			OpaqueMiddle (i);
		try {
			OpaqueMiddle (7);
		} catch (Exception e) {
			Dump ("no-inlining", e);
		}
	}

	/*
	 * Several throws differing only in a constant argument. LLVM's cross-jumping
	 * will happily fold these into one shared call once the argument is hoisted,
	 * at which point a nearest-preceding-marker lookup can only recover whichever
	 * site survived - so this is the fixture for that staying blocked.
	 */
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
	static void ScenarioMergeableThrows ()
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

	/*
	 * Two inlinable bodies stacked, so that once tier 1 folds both into Level1 a
	 * single native offset stands for three methods. That is the only fixture
	 * here that exercises an inline chain deeper than one - a chain the runtime
	 * reports in the wrong order, or truncates to its innermost body, still looks
	 * right with only one level to get wrong.
	 */
	static int Level3 (int x)
	{
		int p1 = x + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
		int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		if (x == 7)
			throw new InvalidOperationException ("level3");
		return x + 9;
	}

	/*
	 * Padded on a constant rather than on the argument, unlike every other fixture
	 * here. The two inliners measure different things: mono's goes by raw IL length,
	 * so this stays out of its reach, while LLVM's cost model folds the whole run to
	 * a constant and charges nothing for it. That is what leaves LLVM room to take
	 * Level2 *and* the Level3 already folded into it, which is what makes the chain
	 * two deep rather than one.
	 */
	static int Level2 (int x)
	{
		int c = 1;
		c += 2; c += 3; c += 4; c += 5; c += 6; c += 7; c += 8; c += 9;
		c += 10; c += 11; c += 12; c += 13; c += 14; c += 15; c += 16;
		c += 17; c += 18; c += 19; c += 20; c += 21; c += 22; c += 23;
		c += 24; c += 25; c += 26; c += 27; c += 28; c += 29; c += 30;
		if (c == 0)
			return -1;
		return Level3 (x);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Level1 (int x)
	{
		return Level2 (x) + 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ScenarioNestedInline ()
	{
		for (int i = 0; i < 3; i++)
			Level1 (i);
		try {
			Level1 (7);
		} catch (Exception e) {
			Dump ("nested-inline", e);
		}
	}

	/*
	 * The same question for a trace taken off the running stack rather than out of
	 * an exception. It reaches the runtime through a different icall
	 * (ves_icall_get_frame_info, one frame per call, driven by a skip count) than
	 * the exception traces above, so an inlined body can be missing from one and
	 * present in the other.
	 *
	 * Offsets are not compared for these - see DumpLive.
	 */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void LiveLeaf ()
	{
		DumpLive ("live-inlined", new StackTrace (false));
	}

	/*
	 * The body that gets inlined has to sit between the capture and the frame we
	 * want it under, not do the capturing itself - constructing a StackTrace costs
	 * more than the inliner will spend, so a capturing method never gets folded in
	 * and the scenario would prove nothing. Padded on a constant, as Level2 is, so
	 * that only LLVM's inliner takes it.
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
	static void ScenarioLiveStackTrace ()
	{
		for (int i = 0; i < 3; i++)
			LiveCaller (i);
		LiveCaller (7);
	}

	/* A throw from inside a finally-bearing frame, which unwinds differently. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ThrowUnderFinally (int x)
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
	static void ScenarioThrowUnderFinally ()
	{
		for (int i = 0; i < 3; i++)
			ThrowUnderFinally (i);
		try {
			ThrowUnderFinally (7);
		} catch (Exception e) {
			Dump ("throw-under-finally", e);
		}
	}

	public static int Main ()
	{
		ScenarioInlinedCallee ();
		ScenarioSmallInline ();
		ScenarioNoInlining ();
		ScenarioMergeableThrows ();
		ScenarioNestedInline ();
		ScenarioLiveStackTrace ();
		ScenarioThrowUnderFinally ();
		return 0;
	}
}
