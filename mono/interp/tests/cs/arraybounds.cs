// Arrays whose element addresses are not a plain scaled index: several ranks,
// non-zero lower bounds, and a rank-1 array that is still a general array
// because its lower bound is not zero.

using System;
using System.Runtime.CompilerServices;

public class ArrayBounds {

	[MethodImpl (MethodImplOptions.NoInlining)] static int Id (int x) { return x; }

	public static int test_7_rank_two_get_and_set ()
	{
		int [,] a = new int [3, 4];
		a [Id (1), Id (2)] = 7;
		return a [1, 2];
	}

	public static int test_9_rank_three_get_and_set ()
	{
		int [,,] a = new int [2, 3, 4];
		a [Id (1), Id (2), Id (3)] = 9;
		return a [1, 2, 3];
	}

	public static int test_24_rank_two_length ()
	{
		int [,] a = new int [4, 6];
		return a.Length;
	}

	public static int test_3_rank_three_rank ()
	{
		int [,,] a = new int [1, 1, 1];
		return a.Rank;
	}

	public static int test_1_rank_two_index_out_of_range ()
	{
		int [,] a = new int [2, 2];
		try {
			a [Id (0), Id (2)] = 1;
			return 0;
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_rank_two_negative_index ()
	{
		int [,] a = new int [2, 2];
		try {
			return a [Id (0), Id (-1)] == 0 ? 0 : 0;
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	// A lower bound that is not zero makes even a rank-1 array take the general
	// index calculation rather than the fast one.
	public static int test_5_non_zero_lower_bound ()
	{
		Array a = Array.CreateInstance (typeof (int), new int [] { 3 }, new int [] { 10 });
		a.SetValue (5, 11);
		return (int) a.GetValue (11);
	}

	public static int test_10_non_zero_lower_bound_reports_it ()
	{
		Array a = Array.CreateInstance (typeof (int), new int [] { 3 }, new int [] { 10 });
		return a.GetLowerBound (0);
	}

	public static int test_1_non_zero_lower_bound_rejects_zero ()
	{
		Array a = Array.CreateInstance (typeof (int), new int [] { 3 }, new int [] { 10 });
		try {
			a.SetValue (1, 0);
			return 0;
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_6_rank_two_non_zero_lower_bounds ()
	{
		Array a = Array.CreateInstance (typeof (int),
			new int [] { 2, 2 }, new int [] { 5, 7 });
		a.SetValue (6, 6, 8);
		return (int) a.GetValue (6, 8);
	}

	public static int test_8_struct_element_in_rank_two ()
	{
		ArrayBoundsCell [,] a = new ArrayBoundsCell [2, 2];
		a [1, 1].Value = 8;
		return a [1, 1].Value;
	}

	public static int test_4_reference_element_in_rank_two ()
	{
		string [,] a = new string [2, 2];
		a [Id (1), Id (0)] = "four";
		return a [1, 0].Length;
	}

	public static int test_3_jagged_of_rank_two ()
	{
		int [][,] a = new int [2][,];
		a [1] = new int [2, 2];
		a [1][0, 1] = 3;
		return a [1][0, 1];
	}

	public static int test_2_rank_two_address_taken ()
	{
		ArrayBoundsCell [,] a = new ArrayBoundsCell [2, 2];
		Fill (ref a [1, 0]);
		return a [1, 0].Value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Fill (ref ArrayBoundsCell cell) { cell.Value = 2; }

	public static int test_1_rank_two_of_wrong_type_throws ()
	{
		object [,] a = new string [2, 2];
		try {
			a [0, 0] = new ArrayBounds ();
			return 0;
		} catch (ArrayTypeMismatchException) {
			return 1;
		}
	}

	public static int test_0_zero_length_rank_two ()
	{
		int [,] a = new int [0, 5];
		return a.Length;
	}
}

public struct ArrayBoundsCell {
	public int Value;
}
