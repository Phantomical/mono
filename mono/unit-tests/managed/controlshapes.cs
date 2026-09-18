// Control flow shapes: switches, loops, goto, and the merge points between
// them.  Each test is about the shape the transform sees rather than one
// opcode.  The cases here are dense and sparse switch tables, loops with an
// early exit, branches out of protected blocks, and blocks that two
// predecessors reach with a value already on the stack.
//
// The value a shape branches on comes through a NoInlining helper.  The
// transform folds constants and inlines short callees, so a shape written over
// literals can compile to one ldc.i4 and test nothing.

using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

[NoOpt]
public class ControlShapes {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static uint IdU (uint x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long IdL (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static char IdC (char x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static string IdS (string x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static ControlShapesLevel IdE (ControlShapesLevel x) { return x; }

	enum ControlShapesLevel { Zero, One, Two, Three, Four }

	static int calls;

	[MethodImpl (MethodImplOptions.NoInlining)] static bool Bump () { calls++; return true; }
	[MethodImpl (MethodImplOptions.NoInlining)] static int Add (int a, int b) { return a + b; }

	// ---------------------------------------------------------------- switch

	static int Dense (int v)
	{
		switch (v) {
		case 0: return 10;
		case 1: return 11;
		case 2: return 12;
		case 3: return 13;
		case 4: return 14;
		case 5: return 15;
		case 6: return 16;
		case 7: return 17;
		default: return 99;
		}
	}

	public static int test_14_switch_dense_takes_its_arm ()
	{
		return Dense (Id (4));
	}

	public static int test_99_switch_dense_above_the_table ()
	{
		return Dense (Id (8));
	}

	public static int test_99_switch_dense_below_the_table ()
	{
		// The interpreter compares the table index unsigned.  A negative value
		// is therefore more than the arm count, not less.
		return Dense (Id (-1));
	}

	static int Biased (int v)
	{
		switch (v) {
		case 10: return 1;
		case 11: return 2;
		case 12: return 3;
		case 13: return 4;
		default: return 9;
		}
	}

	public static int test_3_switch_biased_takes_its_arm ()
	{
		return Biased (Id (12));
	}

	public static int test_9_switch_biased_below_the_lowest_case ()
	{
		// A table that does not start at zero costs a subtraction of the
		// lowest case first.  5 - 10 wraps to a very large unsigned index.
		return Biased (Id (5));
	}

	public static int test_2_switch_negative_cases ()
	{
		switch (Id (-2)) {
		case -3: return 1;
		case -2: return 2;
		case -1: return 3;
		case 0: return 4;
		default: return 9;
		}
	}

	static int Unsigned (uint v)
	{
		switch (v) {
		case 0xfffffffdu: return 1;
		case 0xfffffffeu: return 2;
		case 0xffffffffu: return 3;
		default: return 9;
		}
	}

	public static int test_3_switch_at_the_top_of_the_unsigned_range ()
	{
		return Unsigned (IdU (0xffffffffu));
	}

	public static int test_9_switch_unsigned_index_wraps ()
	{
		return Unsigned (IdU (0u));
	}

	public static int test_7_switch_with_empty_arms ()
	{
		// An empty arm goes to the instruction after the switch.  The other
		// arms return, so the answer names the entry the table took.
		switch (Id (2)) {
		case 0: break;
		case 1: return 1;
		case 2: break;
		case 3: return 3;
		default: return 9;
		}
		return 7;
	}

	public static int test_5_switch_without_a_default ()
	{
		int r = 5;

		switch (Id (9)) {
		case 0: r = 1; break;
		case 1: r = 2; break;
		case 2: r = 3; break;
		case 3: r = 4; break;
		}
		return r;
	}

	static int Shared (int v)
	{
		// Three cases with one body compile to a range test, not a table.
		// Eight cases give a table, and several entries then hold one target.
		switch (v) {
		case 0:
		case 1:
		case 2: return 6;
		case 3: return 7;
		case 4:
		case 5:
		case 6:
		case 7: return 8;
		default: return 9;
		}
	}

	public static int test_6_switch_arms_share_a_body ()
	{
		return Shared (Id (2));
	}

	public static int test_8_switch_shares_a_second_body ()
	{
		return Shared (Id (6));
	}

	static int Level (ControlShapesLevel l)
	{
		switch (l) {
		case ControlShapesLevel.Zero: return 1;
		case ControlShapesLevel.One: return 2;
		case ControlShapesLevel.Two: return 3;
		case ControlShapesLevel.Three: return 4;
		case ControlShapesLevel.Four: return 5;
		default: return 9;
		}
	}

	public static int test_3_switch_on_an_enum ()
	{
		return Level (IdE (ControlShapesLevel.Two));
	}

	public static int test_3_switch_on_a_char ()
	{
		switch (IdC ('c')) {
		case 'a': return 1;
		case 'b': return 2;
		case 'c': return 3;
		case 'd': return 4;
		default: return 9;
		}
	}

	static int Sparse (int v)
	{
		// These cases are too far apart for a table.  The compiler emits a
		// tree of comparisons instead.
		switch (v) {
		case 1: return 1;
		case 50: return 2;
		case 500: return 3;
		case 5000: return 4;
		default: return 9;
		}
	}

	public static int test_3_switch_sparse_takes_its_arm ()
	{
		return Sparse (Id (500));
	}

	public static int test_9_switch_sparse_misses_every_arm ()
	{
		return Sparse (Id (51));
	}

	public static int test_2_switch_on_a_long ()
	{
		switch (IdL (5000000001L)) {
		case 5000000000L: return 1;
		case 5000000001L: return 2;
		case 5000000002L: return 3;
		default: return 9;
		}
	}

	public static int test_2_switch_on_a_short_string_set ()
	{
		switch (IdS ("bb")) {
		case "a": return 1;
		case "bb": return 2;
		case "ccc": return 3;
		default: return 9;
		}
	}

	public static int test_6_switch_on_a_large_string_set ()
	{
		// Past a handful of cases the compiler hashes the string first.  A tree
		// of comparisons finds the hash, and a string equality test then
		// confirms the arm.
		switch (IdS ("f6")) {
		case "a1": return 1;
		case "b2": return 2;
		case "c3": return 3;
		case "d4": return 4;
		case "e5": return 5;
		case "f6": return 6;
		case "g7": return 7;
		case "h8": return 8;
		default: return 9;
		}
	}

	public static int test_4_switch_on_a_null_string ()
	{
		switch (IdS (null)) {
		case "a": return 1;
		case "bb": return 2;
		case "ccc": return 3;
		case null: return 4;
		default: return 9;
		}
	}

	static int Jumping (int v)
	{
		int r = 0;

		switch (v) {
		case 0: r += 1; goto case 2;
		case 1: r += 2; goto default;
		case 2: r += 4; break;
		default: r += 8; break;
		}
		return r;
	}

	public static int test_5_goto_case_enters_another_arm ()
	{
		return Jumping (Id (0));
	}

	public static int test_10_goto_default_enters_the_default_arm ()
	{
		return Jumping (Id (1));
	}

	public static int test_19_switch_inside_a_loop ()
	{
		int total = 0;

		for (int i = Id (0); i < 10; i++) {
			switch (i % 3) {
			case 0: total += 1; break;
			case 1: total += 2; break;
			case 2: total += 3; break;
			}
		}
		return total;
	}

	// ----------------------------------------------------------------- loops

	public static int test_45_nested_loops ()
	{
		int total = 0;

		for (int i = Id (0); i < 5; i++)
			for (int j = Id (0); j < 3; j++)
				total += i + j;
		return total;
	}

	public static int test_10_loop_with_a_break ()
	{
		int total = 0;

		for (int i = Id (0); i < 100; i++) {
			if (i == 10)
				break;
			total += 1;
		}
		return total;
	}

	public static int test_25_loop_with_a_continue ()
	{
		int total = 0;

		for (int i = Id (0); i < 10; i++) {
			if ((i & 1) == 0)
				continue;
			total += i;
		}
		return total;
	}

	public static int test_6_break_leaves_the_inner_loop_only ()
	{
		int total = 0;

		for (int i = Id (0); i < 3; i++) {
			for (int j = Id (0); j < 5; j++) {
				if (j == 2)
					break;
				total += 1;
			}
		}
		return total;
	}

	public static int test_1_do_while_runs_its_body_once ()
	{
		int n = 0;

		do {
			n++;
		} while (Id (0) != 0);
		return n;
	}

	public static int test_10_do_while_counts_down ()
	{
		int i = Id (4), total = 0;

		do {
			total += i;
			i--;
		} while (i > 0);
		return total;
	}

	public static int test_0_for_loop_with_no_trips ()
	{
		int total = 0;

		for (int i = Id (0); i < Id (0); i++)
			total += 1;
		return total;
	}

	public static int test_12_while_true_until_a_break ()
	{
		int i = Id (0), total = 0;

		while (true) {
			if (i == 4)
				break;
			total += 3;
			i++;
		}
		return total;
	}

	public static int test_15_foreach_over_an_array ()
	{
		int[] values = { 1, 2, 3, 4, 5 };
		int total = Id (0);

		foreach (int v in values)
			total += v;
		return total;
	}

	public static int test_15_foreach_over_a_list ()
	{
		List<int> values = new List<int> { 1, 2, 3, 4, 5 };
		int total = Id (0);

		foreach (int v in values)
			total += v;
		return total;
	}

	public static int test_10_foreach_over_a_string ()
	{
		int total = Id (0);

		foreach (char c in IdS ("abcde"))
			total += c - 'a';
		return total;
	}

	public static int test_8_continue_inside_try_finally ()
	{
		int total = 0, marks = 0;

		for (int i = Id (0); i < 4; i++) {
			try {
				if ((i & 1) == 0)
					continue;
				total += i;
			} finally {
				marks++;
			}
		}
		return total + marks;
	}

	public static int test_43_break_out_of_try_runs_the_finally ()
	{
		int total = 0;

		for (int i = Id (0); i < 8; i++) {
			try {
				if (i == 3)
					break;
				total += 1;
			} finally {
				total += 10;
			}
		}
		return total;
	}

	// ------------------------------------------------------------------ goto

	public static int test_4_goto_forward_skips_code ()
	{
		int r = Id (1);

		if (r > 0)
			goto skip;
		r += 100;
	skip:
		r += 3;
		return r;
	}

	public static int test_10_goto_backward_is_a_loop ()
	{
		int i = Id (0), total = 0;

	top:
		total += i;
		i++;
		if (i < 5)
			goto top;
		return total;
	}

	public static int test_3_goto_leaves_two_loops ()
	{
		int total = 0;

		for (int i = Id (0); i < 4; i++) {
			for (int j = Id (0); j < 4; j++) {
				if (i + j == 3)
					goto done;
				total += 1;
			}
		}
	done:
		return total;
	}

	public static int test_11_goto_out_of_try_runs_the_finally ()
	{
		int total = 0;

		try {
			total += 1;
			goto after;
		} finally {
			total += 10;
		}
	after:
		return total;
	}

	public static int test_5_goto_out_of_a_catch ()
	{
		int total = 0;

		try {
			throw new InvalidOperationException ();
		} catch (InvalidOperationException) {
			total += 5;
			goto after;
		}
	after:
		return total;
	}

	// -------------------------------------------------- merges and shortcuts

	public static int test_30_ternary_chain ()
	{
		int v = Id (2);

		return v == 0 ? 10 : v == 1 ? 20 : v == 2 ? 30 : 40;
	}

	public static int test_7_ternary_as_a_call_argument ()
	{
		// Control reaches the second merge point with the first argument
		// already on the stack, so that block starts with a live stack.
		int a = Id (2);

		return Add (a > 0 ? 3 : 5, a > 1 ? 4 : 6);
	}

	public static int test_1_and_short_circuits ()
	{
		calls = 0;
		bool ok = Id (0) != 0 && Bump ();
		return !ok && calls == 0 ? 1 : 0;
	}

	public static int test_1_or_short_circuits ()
	{
		calls = 0;
		bool ok = Id (1) != 0 || Bump ();
		return ok && calls == 0 ? 1 : 0;
	}

	public static int test_40_many_basic_blocks ()
	{
		int v = Id (40), r = 0;

		if (v > 0) r++; if (v > 1) r++; if (v > 2) r++; if (v > 3) r++;
		if (v > 4) r++; if (v > 5) r++; if (v > 6) r++; if (v > 7) r++;
		if (v > 8) r++; if (v > 9) r++; if (v > 10) r++; if (v > 11) r++;
		if (v > 12) r++; if (v > 13) r++; if (v > 14) r++; if (v > 15) r++;
		if (v > 16) r++; if (v > 17) r++; if (v > 18) r++; if (v > 19) r++;
		if (v > 20) r++; if (v > 21) r++; if (v > 22) r++; if (v > 23) r++;
		if (v > 24) r++; if (v > 25) r++; if (v > 26) r++; if (v > 27) r++;
		if (v > 28) r++; if (v > 29) r++; if (v > 30) r++; if (v > 31) r++;
		if (v > 32) r++; if (v > 33) r++; if (v > 34) r++; if (v > 35) r++;
		if (v > 36) r++; if (v > 37) r++; if (v > 38) r++; if (v > 39) r++;
		if (v > 40) r++; if (v > 41) r++; if (v > 42) r++; if (v > 43) r++;
		if (v > 44) r++; if (v > 45) r++; if (v > 46) r++; if (v > 47) r++;
		if (v > 48) r++; if (v > 49) r++; if (v > 50) r++; if (v > 51) r++;
		if (v > 52) r++; if (v > 53) r++; if (v > 54) r++; if (v > 55) r++;
		if (v > 56) r++; if (v > 57) r++; if (v > 58) r++; if (v > 59) r++;
		if (v > 60) r++; if (v > 61) r++; if (v > 62) r++; if (v > 63) r++;
		return r;
	}
}
