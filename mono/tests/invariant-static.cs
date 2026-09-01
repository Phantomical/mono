using System;
using System.Reflection;
using System.Runtime.CompilerServices;

/*
 * Reads of an initonly static. Every class below declares a cctor, so every
 * one translates through push_guarded_static_read (), a runtime branch on
 * the type initializer's own flag rather than a translate-time answer - see
 * class_has_no_cctor () for why. ClassInitWarmPass collapses that branch to
 * its llvm.invariant.start arm once the runtime says the class has finished
 * initializing, which is usually already true by the time a case here is
 * compiled.
 *
 * Each case puts an opaque call between two reads of one field. A wrong
 * invariant mark then answers from the first read instead of taking a second
 * one. A mutable static beside each initonly one is the control: the two are
 * read through the same code, and only one of them is safe to share.
 *
 * Every case runs interpreted and compiled in one process, because the interpreter
 * marks nothing and answers each read from memory. The guarded cases additionally
 * force a compile, with Mono.Tiering.MonoTier::PromoteNow, before anything touches
 * the class they read. ClassInitWarmPass then has nothing to collapse, so the
 * branch these two cases run at runtime is the one this file gates.
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

struct Point {
	public int X;
	public int Y;

	public Point (int x, int y)
	{
		X = x;
		Y = y;
	}
}

class Shapes {
	public static readonly int Scalar = 42;
	public static readonly string Text = "hello";
	public static readonly int[] Numbers = new int[4];
	public static readonly Point Vector = new Point (3, 4);
	public static int Mutable = 7;
}

// Read only through the guarded cases below, never before those force a compile.
// Its own class-init guard is still standing when they translate.
class Guarded {
	public static readonly int Scalar = 42;
	public static readonly Point Vector = new Point (3, 4);
}

/*
 * A type initializer that reads its own static before it assigns it.
 *
 * mono_runtime_class_init_full () lets the initializing thread past the guard
 * without waiting, so the read inside Probe () happens while Value is still zero.
 * A body marked here shares that read with the one after the assignment, and
 * SeenAfter then reports zero.
 */
class Reentrant {
	public static readonly int Value;
	public static readonly int SeenBefore;
	public static readonly int SeenAfter;

	static Reentrant ()
	{
		SeenBefore = Probe ();
		Value = 99;
		SeenAfter = Probe ();
	}

	static int Probe ()
	{
		return Value;
	}
}

class EmptyArray<T> {
	public static readonly T[] Value = new T[0];
}

public class InvariantStatic {
	static int failures;

	static void Check (string what, int got, int want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
		++failures;
	}

	static void Require (bool ok, string what)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL {0}", what);
		++failures;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Opaque (int x)
	{
		return x;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Bump ()
	{
		Shapes.Mutable++;
	}

	// Both reads answer 42, so a shared read is right and an unshared one is too.
	static int ScalarAcrossCall ()
	{
		int first = Shapes.Scalar;

		Opaque (first);

		return first + Shapes.Scalar;
	}

	// The control. Bump () writes between the reads, so a shared read is wrong.
	static int MutableAcrossCall ()
	{
		int first = Shapes.Mutable;

		Bump ();

		return Shapes.Mutable - first;
	}

	static int TextAcrossCall ()
	{
		int first = Shapes.Text.Length;

		Opaque (first);

		return first + Shapes.Text.Length;
	}

	static int ArrayAcrossCall ()
	{
		int first = Shapes.Numbers.Length;

		Opaque (first);

		return first + Shapes.Numbers.Length;
	}

	// A value type reaches memory through a copy rather than a load, so the mark
	// does not reach it. The answer must still be the one the initializer wrote.
	static int VectorAcrossCall ()
	{
		int first = Shapes.Vector.X;

		Opaque (first);

		return first + Shapes.Vector.Y;
	}

	// One static per instantiation, which is the shape the mark exists for.
	static int EmptyAcrossCall ()
	{
		int first = EmptyArray<string>.Value.Length;

		Opaque (first);

		return first + EmptyArray<string>.Value.Length;
	}

	// Compiled by Main () before anything else touches Guarded, so
	// ClassInitWarmPass leaves the branch push_guarded_static_read () wrote standing.
	static int GuardedScalarAcrossCall ()
	{
		int first = Guarded.Scalar;

		Opaque (first);

		return first + Guarded.Scalar;
	}

	static int GuardedVectorAcrossCall ()
	{
		int first = Guarded.Vector.X;

		Opaque (first);

		return first + Guarded.Vector.Y;
	}

	static void Round ()
	{
		Check ("scalar across a call", ScalarAcrossCall (), 84);
		Check ("mutable across a call", MutableAcrossCall (), 1);
		Check ("string across a call", TextAcrossCall (), 10);
		Check ("array across a call", ArrayAcrossCall (), 8);
		Check ("value type across a call", VectorAcrossCall (), 7);
		Check ("empty array across a call", EmptyAcrossCall (), 0);
	}

	const int tier1 = 2; // MonoTier::tier1, as PromoteNow takes it.

	static void PromoteBeforeFirstTouch (string method)
	{
		MethodInfo m = typeof (InvariantStatic).GetMethod (method,
			BindingFlags.Static | BindingFlags.NonPublic);

		Require (Mono.Tiering.MonoTier.PromoteNow (m.MethodHandle.Value, tier1),
			method + " would compile");
	}

	public static int Main ()
	{
		MethodInfo probe = typeof (Reentrant).GetMethod ("Probe",
			BindingFlags.Static | BindingFlags.NonPublic);

		// Neither Reentrant nor Guarded has been touched yet, so each compiles
		// here with cctor_already_ran () false.
		Require (Mono.Tiering.MonoTier.PromoteNow (probe.MethodHandle.Value, tier1),
			"Probe would compile");
		PromoteBeforeFirstTouch ("GuardedScalarAcrossCall");
		PromoteBeforeFirstTouch ("GuardedVectorAcrossCall");

		Check ("reentrant read before the assignment", Reentrant.SeenBefore, 0);
		Check ("reentrant read after the assignment", Reentrant.SeenAfter, 99);
		Check ("the assignment itself", Reentrant.Value, 99);

		Check ("guarded scalar across a call", GuardedScalarAcrossCall (), 84);
		Check ("guarded value type across a call", GuardedVectorAcrossCall (), 7);

		// The first rounds run interpreted, and the later ones run whatever the
		// thresholds promoted. Both answer through this same code.
		for (int i = 0; i < 200; ++i)
			Round ();

		if (failures != 0) {
			Console.WriteLine ("{0} wrong answers", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
