// Checks that guarding a dispatch on the receiver classes tier 1 recorded at it
// never changes what the dispatch returns, and that at tier 2 the recorded class's
// body is inlined behind the guard.
//
// Each root reaches its dispatch through a parameter, so neither the array rule nor
// the guess names a class for it: the record is the only rule that can.

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

class ProfileDevirt {
	static int failures;

	static void Check (string what, int got, int want)
	{
		if (got != want) {
			Console.WriteLine ("{0}: got {1}, want {2}", what, got, want);
			failures++;
		}
	}

	// MonoTier::tier1 and MonoTier::tier2, as PromoteNow takes them.
	const int tier1 = 3;
	const int tier2 = 4;

	// Set once every root is at tier 2, so that the hot class's body throws and
	// the trace says where that body was compiled.
	static bool explode;

	abstract class Shape {
		public abstract int Area ();
	}

	class Square : Shape {
		public override int Area ()
		{
			if (explode)
				throw new InvalidOperationException ();
			return 4;
		}
	}

	// Enters Square's own Area, so a guard merges it into Square's arm.
	class BigSquare : Square {
	}

	class Circle : Shape {
		public override int Area () { return 3; }
	}

	class Triangle : Shape {
		public override int Area () { return 5; }
	}

	class Hexagon : Shape {
		public override int Area () { return 6; }
	}

	class Oval : Shape {
		public override int Area () { return 7; }
	}

	interface IKind {
		int Kind ();
	}

	struct Small : IKind {
		public int v;

		public int Kind ()
		{
			if (explode)
				throw new InvalidOperationException ();
			return v + 100;
		}
	}

	class Large : IKind {
		public int Kind () { return 7; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int One (Shape s) { return s.Area (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Two (Shape s) { return s.Area (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Many (Shape s) { return s.Area (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Keyed (IKind k) { return k.Kind (); }

	static bool Promote (string name, int tier)
	{
		MethodInfo m = typeof (ProfileDevirt).GetMethod (
			name, BindingFlags.Static | BindingFlags.NonPublic);

		return Mono.Tiering.MonoTier.PromoteNow (m.MethodHandle.Value, tier);
	}

	/// Whether the frame of `callee` reports the same native offset as `root`'s,
	/// which says the inline took the callee's body into the root.
	static bool InlinedInto (Exception e, string callee, string root)
	{
		int in_callee = -1, in_root = -2;

		foreach (StackFrame frame in new StackTrace (e, false).GetFrames ()) {
			MethodBase m = frame.GetMethod ();

			if (m == null)
				continue;
			if (m.Name == callee)
				in_callee = frame.GetNativeOffset ();
			if (m.Name == root && m.DeclaringType == typeof (ProfileDevirt))
				in_root = frame.GetNativeOffset ();
		}

		return in_callee >= 0 && in_callee == in_root;
	}

	static bool Inlined (Func<int> run, string callee, string root)
	{
		try {
			run ();
		} catch (InvalidOperationException e) {
			return InlinedInto (e, callee, root);
		}

		Console.WriteLine ("{0}: did not throw", root);
		failures++;
		return false;
	}

	static void Answers ()
	{
		Shape[] all = { new Square (), new BigSquare (), new Circle (), new Triangle (),
		                new Hexagon (), new Oval () };
		int[] areas = { 4, 4, 3, 5, 6, 7 };

		for (int i = 0; i < all.Length; i++) {
			Check ("One " + all[i].GetType ().Name, One (all[i]), areas[i]);
			Check ("Two " + all[i].GetType ().Name, Two (all[i]), areas[i]);
			Check ("Many " + all[i].GetType ().Name, Many (all[i]), areas[i]);
		}

		Check ("Keyed Small", Keyed (new Small { v = 1 }), 101);
		Check ("Keyed Large", Keyed (new Large ()), 7);
	}

	static int Main ()
	{
		bool guarding = Environment.GetEnvironmentVariable ("MONO_GUARD_PROFILE") != "off";
		string[] roots = { "One", "Two", "Many", "Keyed" };

		foreach (string root in roots) {
			if (!Promote (root, tier1)) {
				Console.WriteLine ("{0}: would not compile at tier 1", root);
				return 1;
			}
		}

		Shape square = new Square (), big = new BigSquare (), circle = new Circle ();
		Shape[] spread = { square, circle, new Triangle (), new Hexagon (), new Oval () };
		IKind small = new Small { v = 1 }, large = new Large ();

		for (int i = 0; i < 1000; i++) {
			One (i % 10 == 0 ? circle : square);
			Two (i % 2 == 0 ? circle : i % 4 == 1 ? square : big);
			Many (spread[i % spread.Length]);
			Keyed (i % 10 == 0 ? large : small);
		}

		foreach (string root in roots) {
			if (!Promote (root, tier2)) {
				Console.WriteLine ("{0}: would not compile at tier 2", root);
				return 1;
			}
		}

		Answers ();

		explode = true;

		bool one = Inlined (() => One (square), "Area", "One");
		bool two = Inlined (() => Two (big), "Area", "Two");
		bool keyed = Inlined (() => Keyed (small), "Kind", "Keyed");

		if (one != guarding || two != guarding || keyed != guarding) {
			Console.WriteLine ("inlined behind the guard: One {0}, Two {1}, Keyed {2}; want {3}",
			                   one, two, keyed, guarding);
			failures++;
		}

		explode = false;

		// A receiver the record never saw takes the dispatch the guard kept.
		Answers ();

		if (failures != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
