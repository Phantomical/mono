// Entering a delegate's target instead of reading it off the delegate.
//
// Two producers name a target. A read of an initonly static names the object
// itself, so the call becomes a direct one. The cache the C# compiler writes for
// a lambda or a method group names a candidate only, because the other arm of
// the merge is a read of a mutable field, so the call becomes a compare against
// the delegate's own entry with the direct call on the arm that matches.
//
// Three layers gate it, because no API reports whether the fold fired:
//
//   - every shape answers the same in both arms, so a wrong target is a wrong
//     value rather than a slower call;
//   - a counter on each target says which arm of a guard ran;
//   - a stack trace says the fold really happened. A folded body owns no code,
//     so its frame reports the offset into the root it was folded at. That works
//     only once the target is inlined, which is why it is asserted at tier 2
//     alone - the fold is tier 2's, and nothing inlines a direct call below it.
//
// MONO_LLVM_JIT_FOLD_DELEGATES=0 is the other arm. The first two layers hold
// there as well; the third is what the two arms disagree about.

using System;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

class DelegateFold {
	static int failures;

	static void Check (string what, int got, int want)
	{
		if (got != want) {
			Console.WriteLine ("{0}: got {1}, want {2}", what, got, want);
			failures++;
		}
	}

	static void Expect (string what, bool got, bool want)
	{
		if (got != want) {
			Console.WriteLine ("{0}: got {1}, want {2}", what, got, want);
			failures++;
		}
	}

	/* MonoTier::tier1 and MonoTier::tier2, as PromoteNow takes them. */
	const int tier1 = 2;
	const int tier2 = 3;

	static int calls;

	static int Twice (int x) { calls++; return x * 2; }
	static int Triple (int x) { calls++; return x * 3; }

	// Every site reaches its delegate through this, so the receiver at the
	// Invoke is whatever the caller built rather than a parameter.
	static int Apply (Func<int, int> f, int x) { return f (x); }

	// The initonly producer: the field names the object, so the target is
	// settled and the call carries no guard.
	static readonly Func<int, int> Settled = Twice;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseSettled (int x) { return Settled (x); }

	// The cached producer, both spellings the C# compilers write.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseCachedLambda (int x) { return Apply (v => { calls++; return v * 2; }, x); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseCachedGroup (int x) { return Apply (Twice, x); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Func<int, int> Opaque (Func<int, int> f) { return f; }

	// One arm names Twice and the other names nothing, so a guard is written
	// against Twice. Reaching Triple is what says the guard misses correctly.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseMixed (bool fresh, int x)
	{
		Func<int, int> f = fresh ? new Func<int, int> (Twice) : Opaque (Triple);
		return f (x);
	}

	// A delegate over an instance method of an ordinary object, which is the
	// receiver shape Roslyn's singleton closure also has.
	class Box {
		public int n;
		public int Scale (int x) { calls++; return x * n; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseBound (int x)
	{
		Box b = new Box ();
		b.n = 5;
		return Apply (b.Scale, x);
	}

	static void One () { calls += 1; }
	static void Ten () { calls += 10; }

	// A combined delegate calls every target in its list. Its method_ptr is
	// null, so the compare a guard writes can never match one.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseMulticast ()
	{
		Action a = One;
		a += Ten;
		calls = 0;
		a ();
		return calls;
	}

	// An ldvirtftn delegate resolves its override when it is called, so the
	// method its construction names is not the one it enters.
	class Base { public virtual int Of (int x) { return x + 1; } }
	class Derived : Base { public override int Of (int x) { return x + 100; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseVirtual (Base b, int x)
	{
		Func<int, int> f = b.Of;
		return f (x);
	}

	static void Values ()
	{
		Check ("settled", UseSettled (7), 14);
		Check ("cached-lambda", UseCachedLambda (7), 14);
		Check ("cached-group", UseCachedGroup (7), 14);
		Check ("mixed-fresh", UseMixed (true, 7), 14);
		Check ("mixed-opaque", UseMixed (false, 7), 21);
		Check ("bound", UseBound (7), 35);
		Check ("multicast", UseMulticast (), 11);
		Check ("virtual-base", UseVirtual (new Base (), 7), 8);
		Check ("virtual-derived", UseVirtual (new Derived (), 7), 107);
	}

	// The target the fold proof reads. It throws, so it leaves a frame in the
	// trace, and it is reached only through a delegate.
	//
	// The exception is made once and kept, so the body is a field read and a
	// throw. A body that made one would be larger than the cost model takes,
	// and then the direct call this writes would stand rather than being
	// folded in - which is the thing the offsets below are read for.
	static readonly Exception Bang = new InvalidOperationException ("boom");

	static int Boom (int x) { throw Bang; }

	static readonly Func<int, int> SettledBoom = Boom;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SettledRoot (int x) { return SettledBoom (x); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CachedRoot (int x) { return Apply (Boom, x); }

	/// Whether Boom's frame reports an offset into \p root, which says the fold
	/// took its body into that method rather than leaving a call.
	static bool FoldedInto (Exception e, string root)
	{
		StackTrace trace = new StackTrace (e, false);
		int in_boom = -1, in_root = -2;

		foreach (StackFrame frame in trace.GetFrames ()) {
			MethodBase m = frame.GetMethod ();

			if (m == null || m.DeclaringType != typeof (DelegateFold))
				continue;
			if (m.Name == "Boom")
				in_boom = frame.GetNativeOffset ();
			if (m.Name == root)
				in_root = frame.GetNativeOffset ();
		}

		return in_boom >= 0 && in_boom == in_root;
	}

	static bool Threw (Func<int, int> run, string root)
	{
		try {
			run (0);
		} catch (InvalidOperationException e) {
			return FoldedInto (e, root);
		}


		Console.WriteLine ("{0}: did not throw", root);
		failures++;
		return false;
	}

	static bool Promote (string name, int tier)
	{
		MethodInfo m = typeof (DelegateFold).GetMethod (
			name, BindingFlags.Static | BindingFlags.NonPublic);

		return Mono.Tiering.MonoTier.PromoteNow (m.MethodHandle.Value, tier);
	}

	static void Main ()
	{
		bool folding = Environment.GetEnvironmentVariable (
			               "MONO_LLVM_JIT_FOLD_DELEGATES") != "0";

		// Warm every shape while it is interpreted, so the caches the compilers
		// wrote hold a delegate before anything is compiled against them.
		for (int i = 0; i < 200; i++)
			Values ();

		foreach (string root in new [] { "SettledRoot", "CachedRoot" }) {
			Func<int, int> run = root == "SettledRoot"
			                             ? new Func<int, int> (SettledRoot)
			                             : new Func<int, int> (CachedRoot);

			for (int i = 0; i < 200; i++)
				Threw (run, root);

			if (!Promote (root, tier1) || !Promote (root, tier2)) {
				Console.WriteLine ("{0}: would not compile", root);
				failures++;
				continue;
			}

			Expect (root + " folded at tier 2", Threw (run, root), folding);
		}

		Values ();

		Console.WriteLine (failures == 0 ? "OK" : "FAILED");
		Environment.Exit (failures == 0 ? 0 : 1);
	}
}
