// The exception machinery: throw and rethrow, the catch, filter and finally
// clauses, and the exceptions the engine raises by itself.  C# has no `fault`
// clause, so that fourth kind is not reachable from here.
//
// A method named test_<n>_<what> is a test, and it passes when it returns <n>.
// Operands go through the Id helpers, which the transform cannot inline, so a
// check an opcode makes cannot be folded away before the opcode runs.
//
// A test that cares about the order the handler bodies run in records it in
// `order`, one decimal digit a mark.  The expected value then reads as the
// sequence.

using System;
using System.Runtime.CompilerServices;

[Instrumented]
public class Exceptions {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long IdL (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static object IdO (object x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static string IdS (string x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static int[] IdArr (int[] x) { return x; }

	class ExcAlpha : Exception { public int Tag; public ExcAlpha (int tag) { Tag = tag; } }
	class ExcBeta : ExcAlpha { public ExcBeta (int tag) : base (tag) { } }
	class ExcGamma : Exception { }

	interface IExcThing { int Value (); }
	class ExcObj { public int Field; }
	class ExcShape { public virtual int Sides () { return 4; } }
	struct ExcPoint { public int X, Y; }

	class ExcBadCctor {
		public static int Value = 1;
		static ExcBadCctor () { throw new ExcGamma (); }
	}

	static int order;

	[MethodImpl (MethodImplOptions.NoInlining)] static void Mark (int n) { order = order * 10 + n; }
	[MethodImpl (MethodImplOptions.NoInlining)] static bool MarkTrue (int n) { Mark (n); return true; }
	[MethodImpl (MethodImplOptions.NoInlining)] static bool MarkFalse (int n) { Mark (n); return false; }

	// The caller does not see this throw, so the transform keeps the code after
	// the call.
	[MethodImpl (MethodImplOptions.NoInlining)] static void Throw (Exception e) { throw e; }

	// ---------------------------------------------------------------- throw

	public static int test_5_catch_by_exact_type ()
	{
		try {
			throw new ExcAlpha (Id (5));
		} catch (ExcAlpha e) {
			return e.Tag;
		}
	}

	public static int test_7_catch_by_base_type ()
	{
		try {
			throw new ExcBeta (Id (7));
		} catch (ExcAlpha e) {
			return e.Tag;
		}
	}

	public static int test_2_first_matching_catch_wins ()
	{
		try {
			throw new ExcBeta (Id (2));
		} catch (ExcGamma) {
			return 1;
		} catch (ExcAlpha e) {
			return e.Tag;
		} catch (Exception) {
			return 3;
		}
	}

	public static int test_3_general_catch_takes_anything ()
	{
		try {
			throw new ExcGamma ();
		} catch (ExcAlpha) {
			return 1;
		} catch {
			return 3;
		}
	}

	public static int test_12_unmatched_catch_falls_to_the_outer_one ()
	{
		int marks = 0;

		try {
			try {
				throw new ExcGamma ();
			} catch (ExcAlpha) {
				marks += 1;
			}
		} catch (ExcGamma) {
			marks += 12;
		}
		return marks;
	}

	public static int test_1_catch_gets_the_thrown_object ()
	{
		ExcAlpha thrown = new ExcAlpha (Id (9));

		try {
			throw thrown;
		} catch (ExcAlpha caught) {
			return (object) caught == (object) thrown && caught.Tag == 9 ? 1 : 0;
		}
	}

	public static int test_1_rethrow_reaches_the_outer_catch ()
	{
		ExcAlpha thrown = new ExcAlpha (Id (4));

		try {
			try {
				throw thrown;
			} catch (ExcAlpha) {
				// A rethrow leaves the try it stands in, so the sibling clause
				// below must not see it.
				throw;
			} catch (Exception) {
				return 0;
			}
		} catch (ExcAlpha caught) {
			return (object) caught == (object) thrown ? 1 : 0;
		}
	}

	public static int test_1_throwing_a_caught_object_again ()
	{
		ExcGamma thrown = new ExcGamma ();

		try {
			try {
				throw thrown;
			} catch (ExcGamma e) {
				throw e;
			}
		} catch (ExcGamma caught) {
			return (object) caught == (object) thrown ? 1 : 0;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ExcThrower (int tag) { throw new ExcAlpha (tag); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ExcPassthrough (int tag) { return ExcThrower (tag); }

	public static int test_8_exception_crosses_a_frame_with_no_clauses ()
	{
		try {
			return ExcPassthrough (Id (8));
		} catch (ExcAlpha e) {
			return e.Tag;
		}
	}

	public static int test_5_catch_inside_a_loop ()
	{
		int caught = 0;

		for (int i = Id (0); i < 5; i++) {
			try {
				Throw (new ExcGamma ());
			} catch (ExcGamma) {
				caught++;
			}
		}
		return caught;
	}

	// A resume restores the frame locals.  A local set before the try must read
	// the same value in the handler.
	public static int test_9_locals_survive_the_handler ()
	{
		int a = Id (4), b = Id (5);

		try {
			Throw (new ExcGamma ());
		} catch (ExcGamma) {
			return a + b;
		}
		return 0;
	}

	public static int test_30_struct_local_survives_the_handler ()
	{
		ExcPoint p;

		p.X = Id (10);
		p.Y = Id (20);
		try {
			Throw (new ExcGamma ());
		} catch (ExcGamma) {
			return p.X + p.Y;
		}
		return 0;
	}

	public static int test_1_throwing_null_gives_a_null_reference ()
	{
		Exception e = (Exception) IdO (null);

		try {
			throw e;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	// -------------------------------------------------------------- finally

	public static int test_21_finally_runs_when_an_exception_passes ()
	{
		order = 0;
		try {
			try {
				Throw (new ExcGamma ());
			} finally {
				Mark (2);
			}
		} catch (ExcGamma) {
			Mark (1);
		}
		return order;
	}

	public static int test_12_finally_runs_on_a_plain_leave ()
	{
		order = 0;
		try {
			Mark (1);
		} finally {
			Mark (2);
		}
		return order;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ExcReturnFromTry ()
	{
		try {
			Mark (1);
			return Id (5);
		} finally {
			Mark (2);
		}
	}

	public static int test_17_return_from_try_runs_the_finally ()
	{
		order = 0;

		int value = ExcReturnFromTry ();

		return value + order;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ExcFinallyWritesTheLocal ()
	{
		int v = Id (5);

		try {
			return v;
		} finally {
			v = Id (99);
		}
	}

	public static int test_5_finally_cannot_change_the_returned_value ()
	{
		return ExcFinallyWritesTheLocal ();
	}

	public static int test_123_nested_finallys_run_innermost_first ()
	{
		order = 0;
		try {
			try {
				try {
					Throw (new ExcGamma ());
				} finally {
					Mark (1);
				}
			} finally {
				Mark (2);
			}
		} catch (ExcGamma) {
			Mark (3);
		}
		return order;
	}

	// The leave exits two protected blocks.  The transform puts one handler call
	// for each of them ahead of the branch.
	public static int test_1234_leave_runs_every_finally_between ()
	{
		order = 0;
		try {
			try {
				Mark (1);
				goto done;
			} finally {
				Mark (2);
			}
		} finally {
			Mark (3);
		}
	done:
		Mark (4);
		return order;
	}

	public static int test_123_leave_out_of_a_catch_runs_the_finally ()
	{
		order = 0;
		try {
			try {
				throw new ExcGamma ();
			} catch (ExcGamma) {
				Mark (1);
			} finally {
				Mark (2);
			}
		} finally {
			Mark (3);
		}
		return order;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ExcReturnFromCatch ()
	{
		try {
			throw new ExcGamma ();
		} catch (ExcGamma) {
			Mark (1);
			return Id (3);
		} finally {
			Mark (2);
		}
	}

	public static int test_15_return_from_catch_runs_the_finally ()
	{
		order = 0;
		return ExcReturnFromCatch () + order;
	}

	public static int test_213_finally_runs_when_the_catch_does_not_match ()
	{
		order = 0;
		try {
			try {
				Throw (new ExcGamma ());
			} catch (ExcAlpha) {
				Mark (9);
			} finally {
				Mark (2);
			}
		} catch (ExcGamma) {
			Mark (1);
		}
		Mark (3);
		return order;
	}

	public static int test_1_exception_in_a_finally_replaces_the_first ()
	{
		try {
			try {
				Throw (new ExcGamma ());
			} finally {
				Throw (new ExcAlpha (Id (1)));
			}
		} catch (ExcAlpha e) {
			return e.Tag;
		} catch (ExcGamma) {
			return 2;
		}
		return 3;
	}

	public static int test_1_exception_in_a_finally_on_the_leave_path ()
	{
		int reached = Id (0);

		try {
			try {
				reached = Id (1);
			} finally {
				Throw (new ExcGamma ());
			}
		} catch (ExcGamma) {
			return reached;
		}
		// The leave completes only if the finally lost its exception.
		return 0;
	}

	// The exception machinery runs the finally.  A second exception raised and
	// caught inside it must not disturb the first one, which continues after.
	public static int test_231_a_finally_can_catch_its_own_exception ()
	{
		order = 0;
		try {
			try {
				Throw (new ExcGamma ());
			} finally {
				try {
					Throw (new ExcAlpha (0));
				} catch (ExcAlpha) {
					Mark (2);
				}
				Mark (3);
			}
		} catch (ExcGamma) {
			Mark (1);
		}
		return order;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void ExcDeep (int depth)
	{
		if (depth == 0)
			throw new ExcGamma ();

		try {
			ExcDeep (depth - 1);
		} finally {
			Mark (1);
		}
	}

	public static int test_11115_finallys_run_in_every_frame ()
	{
		order = 0;
		try {
			ExcDeep (Id (4));
		} catch (ExcGamma) {
			Mark (5);
		}
		return order;
	}

	// The second throw finds its handler before the finally between them runs,
	// so the marks read 1, 3, 2 rather than 1, 2, 3.
	public static int test_1324_throw_from_a_handler ()
	{
		order = 0;
		try {
			try {
				try {
					Throw (new ExcGamma ());
				} catch (ExcGamma) {
					Mark (1);
					Throw (new ExcAlpha (0));
				}
			} finally {
				Mark (3);
			}
		} catch (ExcAlpha) {
			Mark (2);
		}
		Mark (4);
		return order;
	}

	// --------------------------------------------------------------- filter

	public static int test_2_filter_true_selects_the_handler ()
	{
		try {
			Throw (new ExcAlpha (Id (2)));
		} catch (ExcAlpha e) when (e.Tag == 1) {
			return 1;
		} catch (ExcAlpha e) when (e.Tag == 2) {
			return e.Tag;
		}
		return 0;
	}

	public static int test_121_a_false_filter_keeps_searching ()
	{
		order = 0;
		try {
			Throw (new ExcGamma ());
		} catch (ExcGamma) when (MarkFalse (1)) {
			Mark (9);
		} catch (ExcGamma) when (MarkTrue (2)) {
			Mark (1);
		}
		return order;
	}

	// A filter runs before the stack unwinds.  It therefore runs before the
	// finally below it.
	public static int test_123_a_filter_runs_before_the_inner_finally ()
	{
		order = 0;
		try {
			try {
				Throw (new ExcGamma ());
			} finally {
				Mark (2);
			}
		} catch (ExcGamma) when (MarkTrue (1)) {
			Mark (3);
		}
		return order;
	}

	// The handler must read what the filter wrote to a local.  The interpreter
	// runs a filter on a copy of the frame, so it has to copy that back.
	public static int test_7_filter_side_effect_reaches_the_handler ()
	{
		int counter = Id (3);

		try {
			Throw (new ExcGamma ());
		} catch (ExcGamma) when ((counter = counter + 4) > 0) {
			return counter;
		}
		return 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool ExcThrowingFilter () { throw new ExcAlpha (0); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ExcFilterThatThrows ()
	{
		try {
			Throw (new ExcGamma ());
		} catch (ExcGamma) when (ExcThrowingFilter ()) {
			return 1;
		} catch (ExcGamma) {
			return 2;
		}
		return 3;
	}

	// ECMA-335 III.3.34, endfilter: "If an exception is thrown inside the filter
	// block, it is intercepted and a value of exception_continue_search is
	// returned." The search therefore passes the filtered clause, and the plain
	// clause behind it answers, which is 2.
	//
	// Both engines fail this, each in its own way. The interpreter reads the
	// throwing filter as a yes and runs the first handler, so it answers 1 --
	// mono_interp_run_filter () leaves its `retval` uninitialized, and only
	// MINT_ENDFILTER writes it. The compiled engine loses ExcAlpha past the
	// catch below, which never runs, and the process dies. The 4 tells that
	// second defect from the first.
	public static int test_2_an_exception_in_a_filter_continues_the_search ()
	{
		try {
			return ExcFilterThatThrows ();
		} catch (ExcAlpha) {
			return 4;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool ExcCatchingFilter ()
	{
		try {
			Throw (new ExcAlpha (0));
		} catch (ExcAlpha) {
			return true;
		}
		return false;
	}

	public static int test_1_a_filter_can_run_its_own_try_catch ()
	{
		try {
			Throw (new ExcGamma ());
		} catch (ExcGamma) when (ExcCatchingFilter ()) {
			return 1;
		}
		return 0;
	}

	// ------------------------------------------------- raised by the engine

	public static int test_1_null_reference_from_a_field_load ()
	{
		ExcObj o = (ExcObj) IdO (null);

		try {
			return o.Field;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_15_null_reference_from_every_shape ()
	{
		ExcObj o = (ExcObj) IdO (null);
		ExcShape shape = (ExcShape) IdO (null);
		int[] a = IdArr (null);
		int total = 0;

		try { o.Field = Id (1); } catch (NullReferenceException) { total += 1; }
		try { total += shape.Sides (); } catch (NullReferenceException) { total += 2; }
		try { total += a [0]; } catch (NullReferenceException) { total += 4; }
		try { total += a.Length; } catch (NullReferenceException) { total += 8; }
		return total;
	}

	public static int test_15_divide_by_zero_in_every_shape ()
	{
		int total = 0;

		try { total += Id (7) / Id (0); } catch (DivideByZeroException) { total += 1; }
		try { total += (int) (IdL (7) / IdL (0)); } catch (DivideByZeroException) { total += 2; }
		try { total += Id (7) % Id (0); } catch (DivideByZeroException) { total += 4; }
		try {
			uint a = (uint) Id (7), b = (uint) Id (0);
			total += (int) (a / b);
		} catch (DivideByZeroException) { total += 8; }
		return total;
	}

	public static int test_1_min_value_over_minus_one_overflows ()
	{
		int a = Id (int.MinValue), b = Id (-1);

		try {
			return a / b;
		} catch (OverflowException) {
			return 1;
		}
	}

	public static int test_1_min_value_remainder_minus_one_overflows ()
	{
		int a = Id (int.MinValue), b = Id (-1);

		try {
			return a % b;
		} catch (OverflowException) {
			return 1;
		}
	}

	public static int test_7_index_out_of_range_in_every_shape ()
	{
		int[] a = new int [3];
		string s = IdS ("abc");
		int total = 0;

		try { total += a [Id (3)]; } catch (IndexOutOfRangeException) { total += 1; }
		try { a [Id (-1)] = 1; } catch (IndexOutOfRangeException) { total += 2; }
		try { total += s [Id (5)]; } catch (IndexOutOfRangeException) { total += 4; }
		return total;
	}

	public static int test_7_invalid_cast_in_every_shape ()
	{
		int total = 0;

		try { total += ((ExcObj) IdO ("text")).Field; } catch (InvalidCastException) { total += 1; }
		try { total += (int) (long) IdO (Id (3)); } catch (InvalidCastException) { total += 2; }
		try { total += ((IExcThing) IdO (new ExcObj ())).Value (); } catch (InvalidCastException) { total += 4; }
		return total;
	}

	public static int test_1_array_type_mismatch_on_store ()
	{
		object[] a = (object[]) IdO (new string [1]);

		try {
			a [0] = new ExcObj ();
			return 0;
		} catch (ArrayTypeMismatchException) {
			return 1;
		}
	}

	public static int test_15_overflow_in_every_shape ()
	{
		int total = 0;

		try { total += checked (Id (int.MaxValue) + Id (1)); } catch (OverflowException) { total += 1; }
		try {
			uint a = (uint) Id (unchecked ((int) 0x80000000)), b = (uint) Id (4);
			total += (int) checked (a * b);
		} catch (OverflowException) { total += 2; }
		try { total += (int) checked (IdL (long.MinValue) - IdL (1)); } catch (OverflowException) { total += 4; }
		try { total += (int) checked ((uint) IdL (-1)); } catch (OverflowException) { total += 8; }
		return total;
	}

	public static int test_15_runtime_exceptions_match_the_base_class ()
	{
		int[] a = new int [1];
		int total = 0;

		try { total += a [Id (4)]; } catch (Exception) { total += 1; }
		try { total += Id (1) / Id (0); } catch (Exception) { total += 2; }
		try { total += ((ExcObj) IdO (null)).Field; } catch (Exception) { total += 4; }
		try { total += ((ExcObj) IdO ("s")).Field; } catch (Exception) { total += 8; }
		return total;
	}

	public static int test_2_a_throwing_cctor_gives_a_type_initializer ()
	{
		try {
			return ExcBadCctor.Value;
		} catch (TypeInitializationException e) {
			return e.InnerException is ExcGamma ? 2 : 1;
		}
	}

	// A resume must release the localloc space of the frames it left.  The space
	// the handler takes afterwards is therefore its own.
	public static unsafe int test_9_localloc_survives_an_exception ()
	{
		int total = 0;

		try {
			int* p = stackalloc int [Id (4)];
			p [0] = Id (3);
			total += p [0];
			Throw (new ExcGamma ());
		} catch (ExcGamma) {
			total += Id (1);
		}

		int* q = stackalloc int [Id (5)];
		q [4] = Id (5);
		total += q [4];
		return total;
	}
}
