using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * `mono-inline-devirt-return-bonus` on a callee that answers with `this`
 * rather than allocating or forwarding a call. tier2-inline-policy.cs's
 * Make () gates the bonus on an allocation, and tier2-inline-return-forward.cs's
 * Relay () gates it on a further call's sealed return type. AsShape () below
 * allocates nothing and calls nothing, so the fold is
 * leaf_operand_class ()'s Argument branch answering `this` exact because Box
 * is sealed, read through `bound_is_exact ()`.
 *
 * The suite runs twice, once on the default and once with the bonus zeroed,
 * and reads MONO_INLINE_POLICY to know which arm it is in. The trivial
 * pre-pass is off in both (--llvm-opt=-mono-inline-il-limit=0), so a fold
 * this reads is the cost model's.
 *
 * What says a fold happened is the stack trace, the way tier2-inline-cost.cs
 * reads it: a folded body owns no code, so its frame reports the offset into
 * Root () that it was folded at, and a body that was really called reports an
 * offset into itself.
 *
 * AsShape () costs 95 on -mono-inline-cost-full, on either arm -- the bonus
 * changes the budget rather than the cost. The suite names a cold-callsite
 * threshold of 45, which the bonus of 100 takes to 145 in the on arm and
 * leaves at 45 in the off arm, fifty either side of the cost. Re-measure
 * when this starts failing on one arm:
 *
 *   MONO_LLVM_JIT_TRACE=1 mono-sgen \
 *     --llvm-opt=-mono-tier2-threshold=0 \
 *     --llvm-opt=-mono-inline-il-limit=0 \
 *     --llvm-opt=-mono-inline-cost-full \
 *     --llvm-opt=-mono-inline-cold-callsite-threshold=1 \
 *     tier2-inline-return-self.exe
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

interface IShape {
	int Area ();
}

sealed class Box : IShape {
	public int w, h;

	public Box (int a, int b) { w = a; h = b; }

	public int Area () { return w * h + 1; }

	/// Answers with `this` rather than an allocation or a further call --
	/// exact only because Box is sealed, which is what leaf_operand_class ()'s
	/// Argument branch reads through bound_is_exact ().
	public IShape AsShape (bool yes)
	{
		if (yes)
			throw new InvalidOperationException ("asshape");

		return this;
	}
}

static class Program {
	static bool saw_asshape, folded_asshape;

	/// Whether AsShape ()'s frame covers the same code as Root ()'s.
	static bool RunsInsideRoot (Exception e)
	{
		StackTrace st = new StackTrace (e, false);
		int in_asshape = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Box" && m.Name == "AsShape")
				in_asshape = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_asshape >= 0 && in_asshape == in_root;
	}

	/// The warm-up calls all take the early return, so the profile reads the
	/// block below as cold and the model weighs its site against
	/// ColdCallSiteThreshold. That is the range this test calibrates in.
	static int Root (int n, bool throwing)
	{
		int total = n * 2;

		if (n >= 0)
			return total;

		try {
			// The dispatch on the answer is what the return bonus reads.
			total += new Box (n & 3, 2).AsShape (throwing).Area ();
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_asshape |= (e.StackTrace ?? "").Contains ("Box.AsShape");
			folded_asshape |= RunsInsideRoot (e);
		}

		return total;
	}

	static int fails;

	static void Check (bool ok, string what)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	public static int Main ()
	{
		bool bonuses = Environment.GetEnvironmentVariable ("MONO_INLINE_POLICY") != "off";
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		// Tier 1 first and asked for, for the reason tier2-inline-cost.cs gives.
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (-2, true);

		Check (saw_asshape, "AsShape () has a frame before tier 2");
		Check (!folded_asshape, "and it runs in a body of its own before tier 2");

		// Enough calls to leave counts on the tier-1 body.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_asshape = folded_asshape = false;

		Check (want == Root (-2, true), "the answer at tier 2 is the answer before it");
		Check (saw_asshape, "AsShape () still has a frame at tier 2");

		if (bonuses)
			Check (folded_asshape,
				"the return bonus folds a body that answers with `this`");
		else
			Check (!folded_asshape,
				"the return bonus is what folds the body that answers with `this`");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
