using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * `mono-inline-devirt-delegate-arg-bonus`, the threshold bonus for a callee
 * that invokes a parameter the site fills with a delegate whose target the
 * compile can name. It is the argument bonus tier2-inline-policy.cs's
 * Measure () takes with a delegate in place of a class: the target arrives
 * with the argument, so an Invoke the body cannot resolve on its own gets one
 * once the body is folded in.
 *
 * Combine () only invokes cb and never lets it escape, the shape only this
 * bonus flips -- tier2-inline-policy.cs's Measure () takes the class argument
 * bonus instead, because it dispatches on the parameter's class rather than
 * invoking it. The suite runs twice, once on the default and once with the
 * bonus zeroed, and reads MONO_INLINE_POLICY to know which arm it is in. The
 * trivial pre-pass is off in both (--llvm-opt=-mono-inline-il-limit=0), so a
 * fold this reads is the cost model's.
 *
 * What says a fold happened is the stack trace, the way tier2-inline-cost.cs
 * reads it: a folded body owns no code, so its frame reports the offset into
 * Root () that it was folded at, and a body that was really called reports an
 * offset into itself.
 *
 * The site passes `new Func<int, int> (Ops.MakeCallback ())`, a delegate
 * built over another delegate -- MakeCallback () is what gets a newobj
 * naming the private Double into Root () at all, since the cost model
 * folds MakeCallback () itself at this threshold. The C# compiler routes
 * a delegate built over a delegate through Func<int,int>:Invoke, so the
 * outer newobj here names Invoke rather than Double. Invoke is itself a
 * callable method sitting directly in Root (), so no field cache and no
 * merge is needed for the walk to answer it.
 *
 * Combine () costs 190 on -mono-inline-cost-full, on either arm -- the bonus
 * changes the budget rather than the cost. The suite names a cold-callsite
 * threshold of 165, which the bonus of 50 takes to 215 in the on arm and
 * leaves at 165 in the off arm, so both verdicts have 25 either way.
 * Re-measure when this starts failing on one arm:
 *
 *   MONO_LLVM_JIT_TRACE=1 MONO_INLINE_POLICY=off mono-sgen \
 *     --llvm-opt=-mono-tier2-threshold=0 \
 *     --llvm-opt=-mono-inline-il-limit=0 \
 *     --llvm-opt=-mono-inline-cost-full \
 *     --llvm-opt=-mono-inline-cold-callsite-threshold=165 \
 *     --llvm-opt=-mono-inline-devirt-delegate-arg-bonus=0 \
 *     --llvm-opt=-mono-inline-noreturn-free=false \
 *     tier2-inline-delegate-arg.exe
 *
 * -mono-inline-noreturn-free is off in that off arm because Combine ()'s own
 * throw arm is otherwise free regardless of the bonus, which prices it below
 * the threshold on its own and leaves nothing for this suite to separate.
 *
 * -mono-inline-cost-full is what makes the printed cost the whole cost -- the
 * model stops adding once it is past the budget, so the cost a plain run
 * prints is cut off there.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Ops {
	static int Double (int v) { return v * 2; }

	/*
	 * Invokes a parameter the site passes a delegate with a named target
	 * in. The target travels in with the argument, so the two calls below
	 * resolve once the body sits beside it.
	 */
	public static int Combine (Func<int, int> cb, bool yes)
	{
		int total = cb (1) + cb (2) * 3;

		if (yes)
			throw new InvalidOperationException ("combine");

		return total;
	}

	public static Func<int, int> MakeCallback () { return Double; }
}

static class Program {
	static bool saw_combine, folded_combine;

	/// Whether Combine ()'s frame covers the same code as Root ()'s.
	static bool RunsInsideRoot (Exception e)
	{
		StackTrace st = new StackTrace (e, false);
		int in_combine = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Ops" && m.Name == "Combine")
				in_combine = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Program" && m.Name == "Root")
				in_root = f.GetNativeOffset ();
		}

		return in_combine >= 0 && in_combine == in_root;
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
			// The newobj is the one this line writes, not the one inside
			// MakeCallback (). The cost model folds that call too, so
			// MakeCallback ()'s own newobj lands in Root () as well. The C#
			// compiler's own delegate-from-delegate form still gives the
			// newobj here Func<int,int>:Invoke as its target, not Double.
			total += Ops.Combine (new Func<int, int> (Ops.MakeCallback ()), throwing);
		} catch (InvalidOperationException e) {
			total += e.Message.Length;
			saw_combine |= (e.StackTrace ?? "").Contains ("Ops.Combine");
			folded_combine |= RunsInsideRoot (e);
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
		bool bonus = Environment.GetEnvironmentVariable ("MONO_INLINE_POLICY") != "off";
		MethodInfo root = typeof (Program).GetMethod ("Root",
			BindingFlags.Static | BindingFlags.NonPublic);

		// Tier 1 first and asked for, for the reason tier2-inline-cost.cs gives.
		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 1");
			return 1;
		}

		int want = Root (-2, true);

		Check (saw_combine, "Combine () has a frame before tier 2");
		Check (!folded_combine, "and it runs in a body of its own before tier 2");

		// Enough calls to leave counts on the tier-1 body.
		for (int i = 0; i < 20000; ++i)
			Root (4, false);

		if (!Mono.Tiering.MonoTier.PromoteNow (root.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: Root () would not compile at tier 2");
			return 1;
		}

		saw_combine = folded_combine = false;

		Check (want == Root (-2, true), "the answer at tier 2 is the answer before it");
		Check (saw_combine, "Combine () still has a frame at tier 2");

		if (bonus)
			Check (folded_combine,
				"the delegate argument bonus folds a body that invokes a named-target argument");
		else
			Check (!folded_combine,
				"the delegate argument bonus is what folds the body that invokes it");

		Console.WriteLine (fails == 0 ? "OK" : "FAILED");
		return fails == 0 ? 0 : 1;
	}
}
