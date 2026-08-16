// Branches, switches, calls and the exception clauses around them.

using System;

public class ControlFlow {

	static int Id (int x) { return x; }

	public static int test_1_conditional_branch ()
	{
		int a = Id (3);
		return a > 2 ? 1 : 0;
	}

	public static int test_45_loop_accumulates ()
	{
		int total = 0;
		for (int i = Id (0); i < 10; i++)
			total += i;
		return total;
	}

	public static int test_3_switch_takes_its_arm ()
	{
		switch (Id (2)) {
		case 0: return 0;
		case 1: return 1;
		case 2: return 3;
		default: return 4;
		}
	}

	public static int test_7_switch_falls_to_default ()
	{
		switch (Id (99)) {
		case 0: return 0;
		default: return 7;
		}
	}

	static int Recurse (int n)
	{
		return n == 0 ? 0 : n + Recurse (n - 1);
	}

	public static int test_15_recursion ()
	{
		return Recurse (5);
	}

	static int Sum (int a, int b, int c, int d, int e, int f, int g, int h)
	{
		return a + b + c + d + e + f + g + h;
	}

	public static int test_36_many_arguments ()
	{
		return Sum (1, 2, 3, 4, 5, 6, 7, 8);
	}

	interface IShape { int Sides (); }
	class Square : IShape { public int Sides () { return 4; } }
	class Triangle : IShape { public int Sides () { return 3; } }

	public static int test_7_interface_dispatch ()
	{
		IShape[] shapes = { new Square (), new Triangle () };
		int total = 0;

		foreach (IShape shape in shapes)
			total += shape.Sides ();
		return total;
	}

	class Base { public virtual int Value () { return 1; } }
	class Derived : Base { public override int Value () { return 2; } }

	public static int test_2_virtual_dispatch ()
	{
		Base b = new Derived ();
		return b.Value ();
	}

	public static int test_6_finally_runs_on_the_way_out ()
	{
		int total = 0;

		try {
			total += 2;
			return total + Marker (ref total);
		} finally {
			total += 100;
		}
	}

	static int Marker (ref int total) { total += 1; return 4; }

	public static int test_3_finally_runs_when_an_exception_passes ()
	{
		int marks = 0;

		try {
			try {
				throw new InvalidOperationException ();
			} finally {
				marks += 1;
			}
		} catch (InvalidOperationException) {
			marks += 2;
		}
		return marks;
	}

	public static int test_2_filter_selects_the_handler ()
	{
		try {
			throw new ArgumentException ("second");
		} catch (ArgumentException e) when (e.Message == "first") {
			return 1;
		} catch (ArgumentException e) when (e.Message == "second") {
			return 2;
		}
	}

	public static int test_1_null_reference_throws ()
	{
		try {
			string s = null;
			return s.Length;
		} catch (NullReferenceException) {
			return 1;
		}
	}
}
