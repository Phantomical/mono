using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * `mono-inline-devirt-return-bonus` on a callee that forwards a further
 * call's answer rather than allocating its own. tier2-inline-policy.cs's
 * Make () already gates the bonus on a body that allocates and returns what
 * it allocated; Relay () below never allocates at all, so the fold is the
 * call-site mark method-to-llvm/call.cpp writes when a callee's declared
 * return type is sealed, not the allocation mark emit_object_alloc () writes.
 * MakeBox ()'s return type, Box, is what carries that mark to Relay ()'s own
 * return.
 *
 * The suite runs twice, once on the default and once with the bonus zeroed,
 * and reads MONO_INLINE_POLICY to know which arm it is in. The trivial
 * pre-pass is off in both (MONO_LLVM_JIT_INLINE_IL_LIMIT=0), so a fold this
 * reads is the cost model's.
 *
 * What says a fold happened is the stack trace, the way tier2-inline-cost.cs
 * reads it: a folded body owns no code, so its frame reports the offset into
 * Root () that it was folded at, and a body that was really called reports an
 * offset into itself.
 *
 * Relay () costs 130 on -mono-inline-cost-full, on either arm -- the bonus
 * changes the budget rather than the cost. The suite names a cold-callsite
 * threshold of 80, which the bonus of 100 takes to 180 in the on arm and
 * leaves at 80 in the off arm, fifty either side of the cost. Re-measure
 * when this starts failing on one arm:
 *
 *   MONO_LLVM_JIT_TRACE=1 MONO_LLVM_JIT_TIER2_THRESHOLD=0 \
 *   MONO_LLVM_JIT_INLINE_IL_LIMIT=0 mono-sgen \
 *     --llvm-opt=-mono-inline-cost-full \
 *     --llvm-opt=-mono-inline-cold-callsite-threshold=1 \
 *     tier2-inline-return-forward.exe
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
}

static class Shapes {
	[MethodImpl (MethodImplOptions.NoInlining)]
	public static Box MakeBox (int w, int h)
	{
		return new Box (w + 1, h + 1);
	}

	/*
	 * Never allocates and never reads an initonly static -- Box only
	 * reaches this body as MakeBox ()'s answer, and Box is sealed, which is
	 * the shape the call-site mark alone is what flips.
	 */
	public static IShape Relay (int w, int h, bool yes)
	{
		Box made = MakeBox (w, h);

		if (yes)
			throw new InvalidOperationException ("relay");

		return made;
	}
}

static class Program {
	static bool saw_relay, folded_relay;

	/// Whether Relay ()'s frame covers the same code as Root ()'s.
	static bool RunsInsideRoot (Exception e)
	{
		StackTrace st = new StackTrace (e, false);
		int in_relay = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Shapes" && m.Name == "Relay")
				in_relay = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_relay >= 0 && in_relay == in_root;
	}

	/*
	 * The warm-up calls all take the early return, so the profile reads the
	 * block below as cold and the model weighs its site against
	 * ColdCallSiteThreshold. That is the range this test calibrates in.
	 */
	static int Root (int n, bool throwing)
	{
		int total = n * 2;

		if (n >= 0)
			return total;

		try {
			// The dispatch on the answer is what the return bonus reads.
			total += Shapes.Relay (n & 3, 2, throwing).Area ();
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_relay |= (e.StackTrace ?? "").Contains ("Shapes.Relay");
			folded_relay |= RunsInsideRoot (e);
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

		Check (saw_relay, "Relay () has a frame before tier 2");
		Check (!folded_relay, "and it runs in a body of its own before tier 2");

		// Enough calls to leave counts on the tier-1 body.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_relay = folded_relay = false;

		Check (want == Root (-2, true), "the answer at tier 2 is the answer before it");
		Check (saw_relay, "Relay () still has a frame at tier 2");

		if (bonuses)
			Check (folded_relay,
				"the return bonus folds a body that forwards a sealed-return call's answer");
		else
			Check (!folded_relay,
				"the return bonus is what folds the body that forwards the answer");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
