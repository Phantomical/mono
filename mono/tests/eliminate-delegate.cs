// Checks that entering a delegate's target directly, instead of invoking through the
// delegate, never changes what a call returns or throws.
//
// Nothing reports whether the elimination fired, so this proves it two ways: behavior
// has to match whether or not the elimination ran, and, at tier 2, a stack trace
// confirms the target was actually inlined into the caller.

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

class DelegateEliminate {
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

	// MonoTier::tier1 and MonoTier::tier2, as PromoteNow takes them.
	const int tier1 = 3;
	const int tier2 = 4;

	static int calls;

	static int Twice (int x) { calls++; return x * 2; }
	static int Triple (int x) { calls++; return x * 3; }

	static int Apply (Func<int, int> f, int x) { return f (x); }

	static readonly Func<int, int> Settled = Twice;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseSettled (int x) { return Settled (x); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseCachedLambda (int x) { return Apply (v => { calls++; return v * 2; }, x); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseCachedGroup (int x) { return Apply (Twice, x); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Func<int, int> Opaque (Func<int, int> f) { return f; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseMixed (bool fresh, int x)
	{
		Func<int, int> f = fresh ? new Func<int, int> (Twice) : Opaque (Triple);
		return f (x);
	}

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

	class Holder {
		public Func<int, int> f;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseFieldCopy (int x)
	{
		Holder h = new Holder ();
		h.f = Settled;
		return h.f (x);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseFieldCopyMixed (bool useTwice, int x)
	{
		Holder h = new Holder ();
		h.f = useTwice ? Settled : new Func<int, int> (Triple);
		return h.f (x);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseFieldCopyUnset (int x)
	{
		Holder h = new Holder ();
		return h.f (x);
	}

	static void One () { calls += 1; }
	static void Ten () { calls += 10; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseMulticast ()
	{
		Action a = One;
		a += Ten;
		calls = 0;
		a ();
		return calls;
	}

	class Base { public virtual int Of (int x) { return x + 1; } }
	class Derived : Base { public override int Of (int x) { return x + 100; } }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UseVirtual (Base b, int x)
	{
		Func<int, int> f = b.Of;
		return f (x);
	}

	struct Vec3 {
		public float x, y, z;

		public Vec3 (float v) { x = v; y = v; z = v; }
	}

	static void CheckVec (string what, Vec3 got, float want)
	{
		if (got.x != want || got.y != want || got.z != want) {
			Console.WriteLine ("{0}: got ({1},{2},{3}), want ({4},{4},{4})",
			                   what, got.x, got.y, got.z, want);
			failures++;
		}
	}

	static Vec3 TwiceV (int x) { calls++; return new Vec3 (x * 2); }
	static Vec3 TripleV (int x) { calls++; return new Vec3 (x * 3); }

	static Vec3 ApplyV (Func<int, Vec3> f, int x) { return f (x); }

	static readonly Func<int, Vec3> SettledV = TwiceV;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Vec3 UseSettledV (int x) { return SettledV (x); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Vec3 UseCachedGroupV (int x) { return ApplyV (TwiceV, x); }

	class BoxV {
		public float n;
		public Vec3 Scale (int x) { calls++; return new Vec3 (x * n); }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Vec3 UseBoundV (int x)
	{
		BoxV b = new BoxV ();
		b.n = 5;
		return ApplyV (b.Scale, x);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Func<int, Vec3> OpaqueV (Func<int, Vec3> f) { return f; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Vec3 UseMixedV (bool fresh, int x)
	{
		Func<int, Vec3> f = fresh ? new Func<int, Vec3> (TwiceV) : OpaqueV (TripleV);
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
		Check ("field-copy", UseFieldCopy (7), 14);
		Check ("field-copy-mixed-twice", UseFieldCopyMixed (true, 7), 14);
		Check ("field-copy-mixed-triple", UseFieldCopyMixed (false, 7), 21);
		Expect ("field-copy-unset throws NullReferenceException",
		        ThrowsNullReference (UseFieldCopyUnset), true);
		CheckVec ("settled-struct", UseSettledV (7), 14);
		CheckVec ("cached-group-struct", UseCachedGroupV (7), 14);
		CheckVec ("bound-struct", UseBoundV (7), 35);
		CheckVec ("mixed-struct-fresh", UseMixedV (true, 7), 14);
		CheckVec ("mixed-struct-opaque", UseMixedV (false, 7), 21);
	}

	/// Whether `run (7)` throws NullReferenceException.
	static bool ThrowsNullReference (Func<int, int> run)
	{
		try {
			run (7);
		} catch (NullReferenceException) {
			return true;
		}

		return false;
	}

	// Bang is a field rather than a `new` inside Boom, to keep Boom's body a
	// read and a throw - small enough for tier 2 to inline.
	static readonly Exception Bang = new InvalidOperationException ("boom");

	static int Boom (int x) { throw Bang; }

	static readonly Func<int, int> SettledBoom = Boom;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SettledRoot (int x) { return SettledBoom (x); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CachedRoot (int x) { return Apply (Boom, x); }

	static Vec3 BoomV (int x) { throw Bang; }

	static readonly Func<int, Vec3> SettledBoomV = BoomV;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Vec3 SettledRootV (int x) { return SettledBoomV (x); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Vec3 CachedRootV (int x) { return ApplyV (BoomV, x); }

	/// Whether `bang`'s frame reports an offset into `root`, which says the
	/// inline took its body into that method rather than leaving a call.
	static bool InlinedInto (Exception e, string bang, string root)
	{
		StackTrace trace = new StackTrace (e, false);
		int in_bang = -1, in_root = -2;

		foreach (StackFrame frame in trace.GetFrames ()) {
			MethodBase m = frame.GetMethod ();

			if (m == null || m.DeclaringType != typeof (DelegateEliminate))
				continue;
			if (m.Name == bang)
				in_bang = frame.GetNativeOffset ();
			if (m.Name == root)
				in_root = frame.GetNativeOffset ();
		}

		return in_bang >= 0 && in_bang == in_root;
	}

	static bool Threw (Func<int, int> run, string root)
	{
		try {
			run (0);
		} catch (InvalidOperationException e) {
			return InlinedInto (e, "Boom", root);
		}


		Console.WriteLine ("{0}: did not throw", root);
		failures++;
		return false;
	}

	static bool ThrewV (Func<int, Vec3> run, string root)
	{
		try {
			run (0);
		} catch (InvalidOperationException e) {
			return InlinedInto (e, "BoomV", root);
		}


		Console.WriteLine ("{0}: did not throw", root);
		failures++;
		return false;
	}

	static bool Promote (string name, int tier)
	{
		MethodInfo m = typeof (DelegateEliminate).GetMethod (
			name, BindingFlags.Static | BindingFlags.NonPublic);

		return Mono.Tiering.MonoTier.PromoteNow (m.MethodHandle.Value, tier);
	}

	static void Main ()
	{
		bool eliminating = Environment.GetEnvironmentVariable ("MONO_ELIMINATE_DELEGATES") != "off";

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

			Expect (root + " inlined at tier 2", Threw (run, root), eliminating);
		}

		foreach (string root in new [] { "SettledRootV", "CachedRootV" }) {
			Func<int, Vec3> run = root == "SettledRootV"
			                             ? new Func<int, Vec3> (SettledRootV)
			                             : new Func<int, Vec3> (CachedRootV);

			for (int i = 0; i < 200; i++)
				ThrewV (run, root);

			if (!Promote (root, tier1) || !Promote (root, tier2)) {
				Console.WriteLine ("{0}: would not compile", root);
				failures++;
				continue;
			}

			Expect (root + " inlined at tier 2", ThrewV (run, root), eliminating);
		}

		Values ();

		Console.WriteLine (failures == 0 ? "OK" : "FAILED");
		Environment.Exit (failures == 0 ? 0 : 1);
	}
}
