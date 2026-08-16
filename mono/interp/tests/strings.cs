// ldstr, the string opcodes the interpreter has of its own (getchr, strlen,
// newobj_string) and the library operations that reach the string intrinsics.
//
// Operands go through NoInlining helpers: csc folds a concatenation of literals
// into one literal, and the transform folds arithmetic on constant locals, so a
// literal written straight into an operation can remove the work under test.
// Non-ASCII characters are written as \u escapes so that the source encoding
// cannot change what is tested.

using System;
using System.Runtime.CompilerServices;

public class Strings {

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string StrId (string s) { return s; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Id (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static uint UId (uint x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static char ChId (char c) { return c; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static char[] CharsId (char[] a) { return a; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object ObjId (object o) { return o; }

	public static int test_5_ldstr_then_strlen ()
	{
		return StrId ("hello").Length;
	}

	public static int test_1_two_equal_literals_are_one_object ()
	{
		return (object) StrId ("interp-strings-literal") ==
		       (object) StrId ("interp-strings-literal") ? 1 : 0;
	}

	public static int test_0_empty_literal_has_no_length ()
	{
		return StrId ("").Length;
	}

	public static int test_104_getchr_first ()
	{
		return StrId ("hello")[Id (0)];
	}

	public static int test_111_getchr_last ()
	{
		string s = StrId ("hello");
		return s[s.Length - 1];
	}

	public static int test_1_getchr_past_the_end_throws ()
	{
		try {
			return StrId ("hello")[Id (5)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	// The bounds test is one unsigned compare, so a negative index takes the
	// same branch as an index above the length.
	public static int test_1_getchr_negative_index_throws ()
	{
		try {
			return StrId ("hello")[Id (-1)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_getchr_on_null_throws ()
	{
		try {
			return StrId (null)[Id (0)];
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_1_strlen_on_null_throws ()
	{
		try {
			return StrId (null).Length;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_4_length_counts_utf16_units ()
	{
		return StrId ("caf\u00e9").Length;
	}

	public static int test_233_getchr_reads_a_non_ascii_unit ()
	{
		return StrId ("caf\u00e9")[Id (3)];
	}

	public static int test_2_a_surrogate_pair_is_two_units ()
	{
		return StrId ("\ud83d\ude00").Length;
	}

	public static int test_1_getchr_reads_the_high_surrogate ()
	{
		return StrId ("\ud83d\ude00")[Id (0)] == 0xd83d ? 1 : 0;
	}

	public static int test_11_concat_two ()
	{
		return (StrId ("hello") + StrId (" world")).Length;
	}

	public static int test_9_concat_three ()
	{
		return (StrId ("abc") + StrId ("def") + StrId ("ghi")).Length;
	}

	public static int test_6_concat_an_array ()
	{
		string[] parts = { StrId ("ab"), StrId ("cd"), StrId ("ef") };
		return string.Concat (parts).Length;
	}

	public static int test_3_concat_treats_null_as_empty ()
	{
		return (StrId ("abc") + StrId (null)).Length;
	}

	public static int test_4_concat_an_object_calls_tostring ()
	{
		return (StrId ("n=") + ObjId (Id (42))).Length;
	}

	public static int test_1_equal_contents_compare_equal ()
	{
		string built = StrId ("ab") + StrId ("c");
		return built == StrId ("abc") ? 1 : 0;
	}

	public static int test_1_ordinal_and_ignore_case_equality ()
	{
		return !StrId ("abc").Equals (StrId ("ABC")) &&
		       string.Equals (StrId ("Hello"), StrId ("hELLO"),
		                      StringComparison.OrdinalIgnoreCase) ? 1 : 0;
	}

	public static int test_1_compareto_orders_by_content ()
	{
		return StrId ("apple").CompareTo (StrId ("banana")) < 0 ? 1 : 0;
	}

	public static int test_1_compare_ordinal ()
	{
		return string.CompareOrdinal (StrId ("abc"), StrId ("ab") + StrId ("c")) == 0 &&
		       string.CompareOrdinal (StrId ("b"), StrId ("a")) > 0 ? 1 : 0;
	}

	public static int test_1_case_mapping_ascii ()
	{
		return StrId ("hello").ToUpperInvariant () == StrId ("HELLO") &&
		       StrId ("HeLLo").ToLowerInvariant () == StrId ("hello") ? 1 : 0;
	}

	public static int test_1_case_mapping_non_ascii ()
	{
		return StrId ("caf\u00e9").ToUpperInvariant () == StrId ("CAF\u00c9") ? 1 : 0;
	}

	public static int test_1_format_substitutes_and_aligns ()
	{
		return string.Format ("{0}-{1}", Id (7), StrId ("x")) == StrId ("7-x") &&
		       string.Format ("[{0,4}]", Id (7)) == StrId ("[   7]") ? 1 : 0;
	}

	public static int test_1_int_tostring_single_digit ()
	{
		return Id (7).ToString () == StrId ("7") ? 1 : 0;
	}

	public static int test_1_int_and_uint_tostring_extremes ()
	{
		return Id (-1234).ToString () == StrId ("-1234") &&
		       Id (int.MinValue).ToString () == StrId ("-2147483648") &&
		       UId (uint.MaxValue).ToString () == StrId ("4294967295") ? 1 : 0;
	}

	// The digit count takes one step above 100000 and another below it, so the
	// sweep puts values on each side of that split.
	public static int test_1_int_tostring_digit_counts ()
	{
		int[] values = { 0, 9, 10, 99, 100, 9999, 10000, 99999, 100000, 1000000 };
		int[] widths = { 1, 1, 2, 2, 3, 4, 5, 5, 6, 7 };

		for (int i = 0; i < values.Length; i++)
			if (Id (values[i]).ToString ().Length != widths[i])
				return 0;
		return 1;
	}

	public static int test_1_gethashcode_agrees_for_equal_strings ()
	{
		string built = StrId ("hel") + StrId ("lo");
		return built.GetHashCode () == StrId ("hello").GetHashCode () ? 1 : 0;
	}

	public static int test_1_gethashcode_separates_different_strings ()
	{
		return StrId ("hello").GetHashCode () != StrId ("world").GetHashCode () ? 1 : 0;
	}

	// The Marvin hash reads eight bytes a turn and then a tail. A string always
	// gives an even byte count, so the only tails are 0, 2, 4 and 6 bytes:
	// lengths 0 to 3 cover them and lengths 4 and 5 add a turn of the loop.
	public static int test_1_gethashcode_over_every_tail_length ()
	{
		string[] words = { "", "a", "ab", "abc", "abcd", "abcde" };

		for (int i = 0; i < words.Length; i++) {
			// Copied through the characters. Concatenation with the empty
			// string gives back the same object, and the equality below is
			// then a hash compared with itself.
			string copy = new string (CharsId (StrId (words[i]).ToCharArray ()));

			if (copy.GetHashCode () != StrId (words[i]).GetHashCode ())
				return 0;
			// A tail case that drops its last unit gives this length the hash
			// of the length below it.
			if (i > 0 && copy.GetHashCode () == StrId (words[i - 1]).GetHashCode ())
				return 0;
		}
		return 1;
	}

	public static int test_3_new_string_repeats_a_char ()
	{
		return new string (ChId ('x'), Id (3)).Length;
	}

	public static int test_0_new_string_of_zero_length ()
	{
		return new string (ChId ('x'), Id (0)).Length;
	}

	public static int test_1_new_string_from_a_char_array ()
	{
		char[] chars = { 'a', 'b', 'c' };
		return new string (CharsId (chars)) == StrId ("abc") ? 1 : 0;
	}

	public static unsafe int test_1_new_string_from_a_pointer ()
	{
		char[] chars = { 'a', 'b', 'c', 'd' };

		fixed (char *p = CharsId (chars))
			return new string (p, Id (1), Id (2)) == StrId ("bc") ? 1 : 0;
	}

	public static int test_1_new_string_with_a_negative_count_throws ()
	{
		try {
			return new string (ChId ('x'), Id (-1)).Length;
		} catch (ArgumentOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_substring_and_indexof ()
	{
		string s = StrId ("hello world");
		return s.Substring (s.IndexOf (ChId ('w'))) == StrId ("world") &&
		       s.IndexOf (ChId ('z')) == -1 ? 1 : 0;
	}

	public static int test_1_split_and_join ()
	{
		string[] parts = StrId ("a,b,c").Split (ChId (','));
		return parts.Length == 3 && string.Join ("-", parts) == StrId ("a-b-c") ? 1 : 0;
	}

	public static int test_1_trim_and_replace ()
	{
		return StrId ("  a-b  ").Trim ().Replace (StrId ("-"), StrId ("+")) ==
		       StrId ("a+b") ? 1 : 0;
	}

	public static int test_1_startswith_and_endswith_ordinal ()
	{
		string s = StrId ("hello");
		return s.StartsWith (StrId ("he"), StringComparison.Ordinal) &&
		       s.EndsWith (StrId ("lo"), StringComparison.Ordinal) ? 1 : 0;
	}

	public static int test_1_tochararray_copies_the_units ()
	{
		char[] chars = StrId ("caf\u00e9").ToCharArray ();
		return chars.Length == 4 && chars[3] == 0x00e9 ? 1 : 0;
	}

	public static int test_1_isnullorempty ()
	{
		return string.IsNullOrEmpty (StrId (null)) && string.IsNullOrEmpty (StrId ("")) &&
		       !string.IsNullOrEmpty (StrId ("a")) ? 1 : 0;
	}

	// A pinned string points at its first character, not at the object header.
	public static unsafe int test_1_fixed_reads_the_characters ()
	{
		fixed (char *p = StrId ("hello"))
			return p[0] == 'h' && p[4] == 'o' ? 1 : 0;
	}

	// With eight cases csc calls ComputeStringHash and searches on the hash, then
	// makes one op_Equality against the case that search found.
	static int StringSwitch (string s)
	{
		switch (s) {
		case "one": return 1;
		case "two": return 2;
		case "three": return 3;
		case "four": return 4;
		case "five": return 5;
		case "six": return 6;
		case "seven": return 7;
		case "eight": return 8;
		default: return 0;
		}
	}

	public static int test_7_switch_on_a_string ()
	{
		return StringSwitch (StrId ("sev") + StrId ("en"));
	}

	public static int test_1_intern_finds_the_literal ()
	{
		string built = StrId ("interp-strings-") + StrId ("interned");
		return (object) string.Intern (built) ==
		       (object) StrId ("interp-strings-interned") ? 1 : 0;
	}
}
