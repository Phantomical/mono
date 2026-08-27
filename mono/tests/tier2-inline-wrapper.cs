using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Whether a wrapper folds into the method that calls it.
 *
 * A managed-to-native wrapper is a method with IL of its own: load the
 * arguments, call the icall, test for a pending interruption, return. may_fold ()
 * lets one through like any other callee, so what decides it is the cost model.
 * The suite turns the trivial pre-pass off (MONO_LLVM_JIT_INLINE_IL_LIMIT=0), so
 * every fold this reads is the model's.
 *
 * A covariant store is the shape. slots is declared object[] and holds a
 * string[], so the store has to ask mono_helper_stelem_ref_check () whether the
 * value fits, and the wrapper around that icall raises the
 * ArrayTypeMismatchException when it does not. That gives one test both halves:
 * a wrapper folded into Root (), and an exception raised inside the folded body
 * and caught by Root ()'s own clause.
 *
 * The site sits in a cold block -- emit_stelem_ref_check ()
 * (method-to-llvm/arrays.cpp) puts it in one, because a store that fits is the
 * ordinary case. So the suite names a cold-callsite threshold of its own, the
 * way tier2-inline-casts.cs does.
 *
 * What says a fold happened is the same reading tier2-inline-casts.cs takes: a
 * folded body owns no code, so its frame reports the offset into Root () that it
 * was folded at, and a body that was really called reports an offset into
 * itself. The frame count is read beside it, because a fold must not take the
 * wrapper's frame away -- an icall that reads its caller finds it there.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Program {
	static object[] slots = new string[4];

	/* What the last ArrayTypeMismatchException's trace said. */
	static int frames, inner_offset, root_offset;
	static bool caught;

	static void Record (Exception e)
	{
		StackTrace st = new StackTrace (e, false);

		frames = st.FrameCount;
		inner_offset = root_offset = -1;

		if (frames > 0)
			inner_offset = st.GetFrame (0).GetNativeOffset ();

		for (int i = 0; i < frames; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m != null && m.DeclaringType != null
			    && m.DeclaringType.Name == "Program" && m.Name == "Root")
				root_offset = f.GetNativeOffset ();
		}
	}

	static int Root (int n, object v)
	{
		int total = RuntimeHelpers.OffsetToStringData + n;

		try {
			slots[n & 3] = v;
			total += 1;
		} catch (ArrayTypeMismatchException e) {
			caught = true;
			Record (e);
			total += 7;
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
		bool folds = Environment.GetEnvironmentVariable ("MONO_WRAPPER_FOLD") != "off";
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		// Tier 1 first and asked for, for the reason tier2-inline-cost.cs gives.
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		caught = false;
		int want_fits = Root (1, "s");
		int want_throws = Root (2, new object ());

		Check (caught, "the store that does not fit raises at tier 1");
		Check (frames >= 2, "and the wrapper has a frame of its own");
		Check (inner_offset >= 0 && root_offset >= 0 && inner_offset != root_offset,
			"and that frame reports an offset into the wrapper's own code");

		int tier1_frames = frames;

		// Enough calls to leave counts on the tier-1 body.
		for (int i = 0; i < 20000; ++i)
			Root (i, "s");

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		caught = false;

		Check (want_fits == Root (1, "s"), "a store that fits answers the same at tier 2");
		Check (want_throws == Root (2, new object ()),
			"and one that does not answers the same");
		Check (caught, "Root ()'s own clause still catches it");
		Check (frames == tier1_frames, "the wrapper still has a frame at tier 2");

		if (folds)
			Check (inner_offset >= 0 && inner_offset == root_offset,
				"and the folded wrapper reports Root ()'s offset");
		else
			Check (inner_offset >= 0 && inner_offset != root_offset,
				"and a wrapper the model refused reports its own");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
