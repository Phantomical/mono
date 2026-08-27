using System;
using System.Runtime.CompilerServices;

/*
 * Reads of an initonly static. emit_ldsfld () decides the !invariant.load mark for
 * each one, once the type initializer is complete.
 *
 * The mark lets LLVM answer one read from an earlier one, so each case puts a call
 * between two reads of one field. A mutable static beside each initonly one is the
 * control: the two are read through the same code, and only one of them is safe to
 * share. A wrong mark is then a wrong value rather than a slower body.
 *
 * Every case runs interpreted and compiled in one process, because the interpreter
 * marks nothing and answers each read from memory.
 */

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

	static void Round ()
	{
		Check ("scalar across a call", ScalarAcrossCall (), 84);
		Check ("mutable across a call", MutableAcrossCall (), 1);
		Check ("string across a call", TextAcrossCall (), 10);
		Check ("array across a call", ArrayAcrossCall (), 8);
		Check ("value type across a call", VectorAcrossCall (), 7);
		Check ("empty array across a call", EmptyAcrossCall (), 0);
	}

	public static int Main ()
	{
		Check ("reentrant read before the assignment", Reentrant.SeenBefore, 0);
		Check ("reentrant read after the assignment", Reentrant.SeenAfter, 99);
		Check ("the assignment itself", Reentrant.Value, 99);

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
