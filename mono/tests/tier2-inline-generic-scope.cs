using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Task #345 asked which of two shapes the tier-2 cost model specializes when
 * it folds a generic callee. One shape is a closed root reaching a callee
 * the runtime would otherwise compile shared. The other is a root that is
 * itself shared folding a callee it names concretely. A trace confirmed the
 * second: it already works through the same gate as the first, and this
 * locks that answer in.
 *
 * Box<T>.UseConcrete is a shared root that calls Helper<int>.IsInt, a
 * concrete instantiation naming nothing about T. Object and string both run
 * the one compile mini_get_shared_method_full () builds for it.
 * resolve_method () (call.cpp) resolves that call site against UseConcrete's
 * own context, which only substitutes UseConcrete's own T. The literal int
 * Helper<int> names is untouched either way, so the callee it hands back is
 * closed regardless of which T is running. depends_on_context () and
 * may_fold () read the callee alone, so materialize () treats this exactly
 * like a closed root's call to a canonically-shared callee. It translates a
 * copy against Helper<int>.IsInt's own exact instantiation. typeof (U) then
 * folds to a constant with no RGCTX fetch behind it.
 *
 * Box<T>.UseOpen is the same shared root calling Helper<T>.IsInt instead,
 * naming UseOpen's own open T. Substitution cannot close that one - T is
 * whatever instantiation is running - so depends_on_context () still answers
 * yes and the call stays on the dispatch the shared body was built with. That
 * arm is here as the negative control: if a later change ever let this fold
 * anyway, it would need a per-instantiation RGCTX this shared body does not
 * carry.
 *
 * The stack trace says whether a fold happened, the same way
 * tier2-inline-cost.cs reads it. A folded body owns no code, so its frame
 * reports the offset into the root it was folded at. A body that really ran
 * reports an offset into itself.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Helper<U> {
	public static int IsInt (U u, bool blowUp)
	{
		if (blowUp)
			throw new InvalidOperationException ("blew up");

		return typeof (U) == typeof (int) ? 1 : 0;
	}
}

static class Box<T> where T : class {
	public static int UseConcrete (T t, int k, bool blowUp)
	{
		return Helper<int>.IsInt (k, blowUp) != 0 ? k + 1 : k;
	}

	public static int UseOpen (T t, int k, bool blowUp)
	{
		return Helper<T>.IsInt (t, blowUp) != 0 ? k + 1 : k;
	}
}

class Program {
	static bool folded_concrete, ran_concrete;
	static bool folded_open, ran_open;

	static bool RunsInsideRoot (Exception e, string root)
	{
		StackTrace st = new StackTrace (e, false);
		int in_helper = -1, in_root = -2;

		for (int i = 0; i < st.FrameCount; i++) {
			StackFrame f = st.GetFrame (i);
			MethodBase m = f.GetMethod ();

			if (m == null)
				continue;
			if (m.DeclaringType.Name == "Helper`1" && m.Name == "IsInt")
				in_helper = f.GetNativeOffset ();
			if (m.DeclaringType.Name == "Box`1" && m.Name == root)
				in_root = f.GetNativeOffset ();
		}

		return in_helper >= 0 && in_helper == in_root;
	}

	static int fails;

	static void Check (bool ok, string what)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++fails;
	}

	static void RunConcrete<T> (T t) where T : class
	{
		try {
			Box<T>.UseConcrete (t, 4, true);
		} catch (InvalidOperationException e) {
			ran_concrete = true;
			folded_concrete |= RunsInsideRoot (e, "UseConcrete");
		}
	}

	static void RunOpen<T> (T t) where T : class
	{
		try {
			Box<T>.UseOpen (t, 4, true);
		} catch (InvalidOperationException e) {
			ran_open = true;
			folded_open |= RunsInsideRoot (e, "UseOpen");
		}
	}

	public static int Main ()
	{
		MethodInfo useConcrete = typeof (Box<object>).GetMethod ("UseConcrete");
		MethodInfo useOpen = typeof (Box<object>).GetMethod ("UseOpen");

		if (!Mono.Tiering.MonoTier.PromoteNow (useConcrete.MethodHandle.Value, 2)
		    || !Mono.Tiering.MonoTier.PromoteNow (useOpen.MethodHandle.Value, 2)) {
			Console.WriteLine ("FAIL: the roots would not compile at tier 1");
			return 1;
		}

		Check (Box<object>.UseConcrete (new object (), 4, false) == 5,
			"typeof (int) answers true before tier 2");
		Check (Box<object>.UseOpen (new object (), 4, false) == 4,
			"typeof (T) answers false for a reference T before tier 2");

		// The loop runs two reference-type instantiations of the same shared
		// root, so a fold that answered off the wrong T would show up as
		// soon as the second one runs.
		for (int i = 0; i < 20000; i++) {
			Box<object>.UseConcrete (new object (), i, false);
			Box<string>.UseConcrete ("x", i, false);
			Box<object>.UseOpen (new object (), i, false);
			Box<string>.UseOpen ("x", i, false);
		}

		if (!Mono.Tiering.MonoTier.PromoteNow (useConcrete.MethodHandle.Value, 3)
		    || !Mono.Tiering.MonoTier.PromoteNow (useOpen.MethodHandle.Value, 3)) {
			Console.WriteLine ("FAIL: the roots would not compile at tier 2");
			return 1;
		}

		RunConcrete (new object ());
		RunConcrete ("x");
		RunOpen (new object ());
		RunOpen ("x");

		Check (ran_concrete && folded_concrete,
			"the cost model folds a concrete callee out of a shared root");
		Check (ran_open && !folded_open,
			"and leaves an open callee dispatching out of the same root");

		Check (Box<object>.UseConcrete (new object (), 4, false) == 5,
			"typeof (int) still answers true at tier 2");
		Check (Box<string>.UseConcrete ("x", 4, false) == 5,
			"and the same fold answers true for the other instantiation");
		Check (Box<object>.UseOpen (new object (), 4, false) == 4,
			"typeof (T) still answers false for a reference T at tier 2");
		Check (Box<string>.UseOpen ("x", 4, false) == 4,
			"and for the other instantiation too");

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
