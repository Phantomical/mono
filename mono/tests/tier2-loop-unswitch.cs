using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * A branch inside a loop, on a condition the loop's own turns never change,
 * with both arms staying in the loop and the Tag switch giving the loop more
 * than one latch. Only SimpleLoopUnswitchPass's non-trivial mode reaches a
 * branch like this: trivial unswitching only takes one whose arms leave the
 * loop on one side.
 *
 * This checks that tier 2 answers correctly here, not that it once did not.
 * A prior fix (task #299) found real corruption from non-trivial unswitching
 * on a much larger, self-recursive method, but no reduction of that case
 * down to something this small has reproduced it. Isolating
 * SimpleLoopUnswitchPass on the captured IR converges to identical output
 * whether or not non-trivial unswitching runs, so whatever the real defect
 * needed is not present here. Reverting the fix does not fail this test.
 */
abstract class Node {
	protected int tag;

	public int Tag {
		get { return tag; }
	}
}

sealed class Wrapper : Node {
	Node a;
	Node b;
	Node c;

	public Node A {
		get { return a; }
		set { a = value; }
	}

	public Node B {
		get { return b; }
		set { b = value; }
	}

	public Node C {
		get { return c; }
		set { c = value; }
	}

	public Wrapper (int which)
	{
		tag = which;
	}
}

sealed class Leaf : Node {
	public int Value;

	public Leaf (int value)
	{
		tag = 3;
		Value = value;
	}
}

static class Program {
	/* MonoTier::tier2, as PromoteNow takes it. */
	const int tier2 = 3;

	static int steps;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Walk (Node node, bool takeA, int e1, int e2, int e3, int e4, int e5, int e6)
	{
		int acc1 = e1;
		int acc2 = e2;
		int acc3 = e3;

	top:
		switch (node.Tag) {
		case 0:
			if (takeA) {
				node = ((Wrapper) node).A;
			} else {
				++steps;
				node = ((Wrapper) node).B;
			}
			acc1 += e4;
			goto backedge;
		case 1:
			node = ((Wrapper) node).C;
			acc2 += e5;
			goto backedge;
		case 2:
			node = ((Wrapper) node).A;
			acc3 += e6;
			goto backedge;
		default:
			return ((Leaf) node).Value + acc1 + acc2 + acc3;
		}

	backedge:
		goto top;
	}

	static Node Chain (int depth, int value)
	{
		Node node = new Leaf (value);

		for (int i = 0; i < depth; ++i) {
			Wrapper w = new Wrapper (0);
			w.A = node;
			w.B = node;
			node = w;
		}

		return node;
	}

	/// Puts Walk () at tier 2, and says so when the backend declines.
	static bool Promote ()
	{
		MethodInfo target = typeof (Program).GetMethod ("Walk",
			BindingFlags.Static | BindingFlags.NonPublic);

		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier2))
			return true;

		Console.WriteLine ("FAIL: Walk () would not compile at tier 2");
		return false;
	}

	public static int Main ()
	{
		if (!Promote ())
			return 1;

		const int depth = 8;
		Node chain = Chain (depth, 999);
		int expect = 999 + (1 + 4 * depth) + 2 + 3;

		for (int i = 0; i < 10000; ++i) {
			int rTrue = Walk (chain, true, 1, 2, 3, 4, 5, 6);
			int rFalse = Walk (chain, false, 1, 2, 3, 4, 5, 6);

			if (rTrue != expect || rFalse != expect) {
				Console.WriteLine ("FAIL: call {0} got {1}/{2}, wanted {3}",
					i, rTrue, rFalse, expect);
				return 1;
			}
		}

		Console.WriteLine ("OK");
		return 0;
	}
}

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}
