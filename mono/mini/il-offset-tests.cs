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
	 * Padded to ~15 statements for the same reason inliner-tests.cs pads its
	 * fixtures: without it the classic JIT folds the call away before tier 1 ever
	 * sees it, and the scenario stops being about tier-1 inlining.
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
		ScenarioNoInlining ();
		ScenarioMergeableThrows ();
		ScenarioThrowUnderFinally ();
		return 0;
	}
}
