// The public operations that stand in front of the corlib helpers
// transform.c answers with an opcode of its own: string.GetHashCode
// (System.Marvin.Block), Span.Clear over a reference element type
// (SpanHelpers.ClearWithReferences), ordinal ignore-case comparison and
// invariant casing (System.Text.Unicode.Utf16Utility),
// Encoding.ASCII.GetString (System.Text.ASCIIUtility.WidenAsciiToUtf16) and
// uint.ToString (System.Number.UInt32ToDecStr).
//
// Only Marvin and SpanHelpers exist in the net_4_x corlib this assembly
// compiles against.  Utf16Utility and ASCIIUtility are absent, and this
// Number.UInt32ToDecStr takes two arguments where transform.c wants one, so
// four of the six opcodes cannot run here.  Those tests record the answers the
// managed bodies give, which an opcode that stands in for one of them has to
// match.
//
// A test that only asks two answers to agree is not empty.  The mixed arm
// promotes a corlib body after its first call, so one of the two answers can
// come from the interpreter and the other from compiled code.
//
// Every operand comes from a NoInlining helper or from a char array built at
// run time.  csc folds a literal expression, and a folded operand removes the
// work under test.
//
// The strings run to a few dozen characters.  These bodies read the text in
// blocks of two, four or eight units, and a short string never leaves the
// scalar tail.

using System;
using System.Runtime.CompilerServices;
using System.Text;

public class StringIntrins {

	struct StringIntrinsRefPair { public object A; public string B; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Id (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static uint UId (uint x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static char ChId (char c) { return c; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string StrId (string s) { return s; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static byte[] BytesId (byte[] b) { return b; }

	// A string of the given length, built through a char array so that no part of
	// it is a literal the compiler can share or fold.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string Build (int length, int seed)
	{
		char[] chars = new char[length];
		for (int i = 0; i < length; i++)
			chars[i] = (char) ('a' + ((i + seed) % 26));
		return new string (chars);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string BuildUpper (int length, int seed)
	{
		char[] chars = new char[length];
		for (int i = 0; i < length; i++)
			chars[i] = (char) ('A' + ((i + seed) % 26));
		return new string (chars);
	}

	// The same text with the character at one offset in the other case.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string FlipCaseAt (string s, int offset)
	{
		char[] chars = s.ToCharArray ();
		char c = chars[offset];
		if (c >= 'a' && c <= 'z')
			chars[offset] = (char) (c - 32);
		else if (c >= 'A' && c <= 'Z')
			chars[offset] = (char) (c + 32);
		return new string (chars);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string ReplaceAt (string s, int offset, char c)
	{
		char[] chars = s.ToCharArray ();
		chars[offset] = c;
		return new string (chars);
	}

	// Marvin.Block, through string.GetHashCode.  The seed is drawn once per
	// process, so a test can only compare two hashes taken in the same process,
	// never a hash against a fixed value.

	public static int test_1_hash_agrees_for_equal_long_strings ()
	{
		string a = Build (Id (48), Id (0));
		string b = Build (Id (48), Id (0));

		return (object) a != (object) b && a.GetHashCode () == b.GetHashCode () ? 1 : 0;
	}

	public static int test_1_hash_agrees_at_every_length ()
	{
		// Marvin takes four bytes a round.  A string gives an even byte count, so
		// the tail is 0, 2, 4 or 6 bytes, and the sweep varies how many rounds run
		// in front of each of them.
		for (int len = 0; len <= 40; len++) {
			if (Build (len, Id (3)).GetHashCode () != Build (len, Id (3)).GetHashCode ())
				return len + 100;
		}
		return 1;
	}

	public static int test_1_hash_differs_at_every_offset ()
	{
		string baseline = Build (Id (40), Id (1));

		for (int i = 0; i < 40; i++) {
			if (ReplaceAt (baseline, i, '0').GetHashCode () == baseline.GetHashCode ())
				return i + 100;
		}
		return 1;
	}

	public static int test_1_hash_of_a_non_ascii_string ()
	{
		char[] chars = new char[36];
		for (int i = 0; i < chars.Length; i++)
			chars[i] = (char) (0x0410 + i);

		string a = new string (chars);
		string b = new string (chars);

		return a.GetHashCode () == b.GetHashCode () ? 1 : 0;
	}

	public static int test_1_hash_of_one_char_differs_from_empty ()
	{
		// No round runs for either of these: the empty string takes the zero-byte
		// tail and one character takes the two-byte one.
		string empty = Build (Id (0), Id (0));
		string one = Build (Id (1), Id (0));

		return one.GetHashCode () != empty.GetHashCode () ? 1 : 0;
	}

	public static int test_1_hash_of_a_substring_matches ()
	{
		// A substring is a fresh string, so the hash reads from its own first
		// character rather than at an offset into the original.
		string whole = Build (Id (40), Id (2));
		string tail = whole.Substring (Id (7));

		return tail.GetHashCode () == Build (Id (33), Id (9)).GetHashCode () ? 1 : 0;
	}

	public static int test_1_ignore_case_hash_agrees_across_case ()
	{
		string lower = Build (Id (44), Id (0));
		string upper = lower.ToUpperInvariant ();

		StringComparer comparer = StringComparer.OrdinalIgnoreCase;
		return comparer.GetHashCode (lower) == comparer.GetHashCode (upper) ? 1 : 0;
	}

	// Ordinal ignore-case comparison.  A block compare of ASCII masks the case
	// bit only where the unit is a letter, so the characters next to 'A' and 'a'
	// in the table separate a correct mask from a blanket one.

	public static int test_1_ignore_case_equals_long_ascii ()
	{
		string lower = Build (Id (48), Id (0));
		string upper = BuildUpper (Id (48), Id (0));

		return string.Equals (lower, upper, StringComparison.OrdinalIgnoreCase) ? 1 : 0;
	}

	public static int test_1_ignore_case_case_flip_at_every_offset ()
	{
		string baseline = Build (Id (40), Id (4));

		for (int i = 0; i < 40; i++) {
			if (!string.Equals (baseline, FlipCaseAt (baseline, i), StringComparison.OrdinalIgnoreCase))
				return i + 100;
		}
		return 1;
	}

	public static int test_1_ignore_case_change_at_every_offset ()
	{
		string baseline = Build (Id (40), Id (4));

		for (int i = 0; i < 40; i++) {
			if (string.Equals (baseline, ReplaceAt (baseline, i, '7'), StringComparison.OrdinalIgnoreCase))
				return i + 100;
		}
		return 1;
	}

	public static int test_1_ignore_case_only_letters_fold ()
	{
		// c and c+0x20 differ in the same bit that separates 'A' from 'a'.  The
		// pair compares equal only when c is a capital letter.  It sits at offset
		// 32 with seven characters behind it, so a block compare holds it in a
		// full block rather than in the scalar tail.
		string head = Build (Id (32), Id (0));
		string tail = Build (Id (7), Id (5));

		for (int c = 0x21; c <= 0x5e; c++) {
			string a = head + ChId ((char) c) + tail;
			string b = head + ChId ((char) (c + 0x20)) + tail;
			bool equal = string.Equals (a, b, StringComparison.OrdinalIgnoreCase);
			bool expected = c >= 'A' && c <= 'Z';

			if (equal != expected)
				return c;
		}
		return 1;
	}

	public static int test_1_ignore_case_brackets_do_not_fold ()
	{
		// Six pairs that differ by 0x20 and hold no letter, side by side rather
		// than one at a time: [{ \| ]} ^~ _<del> and `@.  Both strings are 22
		// characters, so the compare cannot answer from the length.
		string a = Build (Id (16), Id (0)) + StrId ("[\\]^_`");
		string b = Build (Id (16), Id (0)) + StrId ("{|}~\u007f@");

		// The same six characters after an upper-case head still fold, so a
		// compare stuck on "not equal" cannot pass this.
		if (!string.Equals (a, BuildUpper (Id (16), Id (0)) + StrId ("[\\]^_`"),
				    StringComparison.OrdinalIgnoreCase))
			return 0;

		return string.Equals (a, b, StringComparison.OrdinalIgnoreCase) ? 0 : 1;
	}

	public static int test_1_ignore_case_digits_need_the_same_digit ()
	{
		string a = Build (Id (24), Id (0)) + StrId ("0123456789");
		string copy = Build (Id (24), Id (0)) + StrId ("0123456789");
		string b = Build (Id (24), Id (0)) + StrId ("0123456780");

		if (!string.Equals (a, copy, StringComparison.OrdinalIgnoreCase))
			return 0;

		return string.Equals (a, b, StringComparison.OrdinalIgnoreCase) ? 0 : 1;
	}

	public static int test_1_ignore_case_at_every_length ()
	{
		for (int len = 0; len <= 40; len++) {
			string lower = Build (len, Id (2));
			string upper = BuildUpper (len, Id (2));

			if (!string.Equals (lower, upper, StringComparison.OrdinalIgnoreCase))
				return len + 100;
		}
		return 1;
	}

	public static int test_1_ignore_case_non_ascii_folds_invariantly ()
	{
		// The last three units are outside ASCII, so the compare leaves the block
		// form there and folds them the general way.
		string a = Build (Id (24), Id (0)) + StrId ("\u00C1\u00C9\u00CD");
		string b = BuildUpper (Id (24), Id (0)) + StrId ("\u00E1\u00E9\u00ED");

		return string.Equals (a, b, StringComparison.OrdinalIgnoreCase) ? 1 : 0;
	}

	public static int test_1_ignore_case_non_ascii_differs ()
	{
		string a = Build (Id (24), Id (0)) + StrId ("\u00C1\u00C9\u00CD");
		string b = Build (Id (24), Id (0)) + StrId ("\u00C1\u00C9\u00CE");

		return string.Equals (a, b, StringComparison.OrdinalIgnoreCase) ? 0 : 1;
	}

	public static int test_1_ignore_case_compare_orders ()
	{
		string a = Build (Id (32), Id (0));
		string b = BuildUpper (Id (32), Id (0));
		string later = ReplaceAt (a, Id (31), 'z');

		if (string.Compare (a, b, StringComparison.OrdinalIgnoreCase) != 0)
			return 0;

		return string.Compare (a, later, StringComparison.OrdinalIgnoreCase) < 0 ? 1 : 0;
	}

	public static int test_1_ignore_case_startswith_and_endswith ()
	{
		string s = Build (Id (48), Id (0));
		string head = BuildUpper (Id (20), Id (0));
		string tail = BuildUpper (Id (16), Id (32));

		if (!s.StartsWith (head, StringComparison.OrdinalIgnoreCase))
			return 0;

		return s.EndsWith (tail, StringComparison.OrdinalIgnoreCase) ? 1 : 0;
	}

	public static int test_20_ignore_case_indexof ()
	{
		string s = Build (Id (48), Id (0));
		string needle = BuildUpper (Id (8), Id (20));

		return s.IndexOf (needle, StringComparison.OrdinalIgnoreCase);
	}

	// Invariant casing.  A block conversion takes two units at a time and masks
	// only the ASCII letters in the pair, so an odd length and a non-letter
	// beside a letter separate it from a blanket mask.

	public static int test_1_toupper_invariant_long_ascii ()
	{
		string lower = Build (Id (48), Id (0));
		string upper = lower.ToUpperInvariant ();

		for (int i = 0; i < 48; i++) {
			if (upper[i] != (char) (lower[i] - 32))
				return i + 100;
		}
		return 1;
	}

	public static int test_1_tolower_invariant_long_ascii ()
	{
		string upper = BuildUpper (Id (48), Id (0));
		string lower = upper.ToLowerInvariant ();

		for (int i = 0; i < 48; i++) {
			if (lower[i] != (char) (upper[i] + 32))
				return i + 100;
		}
		return 1;
	}

	public static int test_1_toupper_invariant_at_every_length ()
	{
		for (int len = 0; len <= 20; len++) {
			if (Build (len, Id (0)).ToUpperInvariant () != BuildUpper (len, Id (0)))
				return len + 100;
		}
		return 1;
	}

	public static int test_1_toupper_invariant_leaves_non_letters ()
	{
		string s = StrId ("ab[c\\d]e^f_g`h{i|j}k~l@m0n9o");
		string upper = s.ToUpperInvariant ();

		for (int i = 0; i < s.Length; i++) {
			char c = s[i];
			char expected = (c >= 'a' && c <= 'z') ? (char) (c - 32) : c;
			if (upper[i] != expected)
				return i + 100;
		}
		return 1;
	}

	public static int test_1_toupper_invariant_non_ascii ()
	{
		string s = Build (Id (16), Id (0)) + StrId ("\u00E1\u00E9\u00ED") + Build (Id (16), Id (0));
		string upper = s.ToUpperInvariant ();

		return upper[Id (16)] == '\u00C1' && upper[Id (0)] == 'A' ? 1 : 0;
	}

	public static int test_1_toupper_invariant_round_trip ()
	{
		string s = Build (Id (45), Id (7));
		return s.ToUpperInvariant ().ToLowerInvariant () == s ? 1 : 0;
	}

	// Encoding.ASCII.  GetString widens the bytes to UTF-16 up to the first byte
	// with the high bit set, and the fallback then writes a question mark for it.
	// The offset of that byte decides how much of a block widening runs.

	public static int test_1_ascii_getstring_all_ascii ()
	{
		byte[] bytes = new byte[64];
		for (int i = 0; i < bytes.Length; i++)
			bytes[i] = (byte) ('a' + (i % 26));

		string s = Encoding.ASCII.GetString (BytesId (bytes));

		if (s.Length != 64)
			return 0;

		for (int i = 0; i < 64; i++) {
			if (s[i] != (char) bytes[i])
				return i + 100;
		}
		return 1;
	}

	public static int test_1_ascii_getstring_high_bit_at_every_offset ()
	{
		for (int bad = 0; bad < 40; bad++) {
			byte[] bytes = new byte[40];
			for (int i = 0; i < bytes.Length; i++)
				bytes[i] = (byte) ('a' + (i % 26));
			bytes[bad] = 0xC3;

			string s = Encoding.ASCII.GetString (BytesId (bytes));

			if (s.Length != 40 || s[bad] != '?')
				return bad + 100;
			if (bad > 0 && s[bad - 1] != (char) bytes[bad - 1])
				return bad + 200;
		}
		return 1;
	}

	public static int test_1_ascii_getstring_at_every_length ()
	{
		for (int len = 0; len <= 33; len++) {
			byte[] bytes = new byte[len];
			for (int i = 0; i < len; i++)
				bytes[i] = (byte) ('A' + (i % 26));

			if (Encoding.ASCII.GetString (BytesId (bytes)).Length != len)
				return len + 100;
		}
		return 1;
	}

	public static int test_0_ascii_getstring_of_no_bytes ()
	{
		return Encoding.ASCII.GetString (BytesId (new byte[0])).Length;
	}

	public static int test_1_ascii_getstring_of_a_range ()
	{
		byte[] bytes = new byte[48];
		for (int i = 0; i < bytes.Length; i++)
			bytes[i] = (byte) ('a' + (i % 26));

		string s = Encoding.ASCII.GetString (BytesId (bytes), Id (5), Id (30));

		return s.Length == 30 && s[0] == (char) bytes[5] ? 1 : 0;
	}

	public static int test_1_ascii_round_trip ()
	{
		string s = Build (Id (50), Id (3));
		byte[] bytes = Encoding.ASCII.GetBytes (StrId (s));

		return Encoding.ASCII.GetString (BytesId (bytes)) == s ? 1 : 0;
	}

	public static int test_1_ascii_getbytes_replaces_non_ascii ()
	{
		string s = Build (Id (20), Id (0)) + StrId ("\u00E9") + Build (Id (20), Id (0));
		byte[] bytes = Encoding.ASCII.GetBytes (StrId (s));

		return bytes.Length == 41 && bytes[20] == (byte) '?' ? 1 : 0;
	}

	// uint.ToString.  The decimal conversion counts the digits first, so each
	// digit count and each power-of-ten boundary is a separate arm of that count.

	public static int test_1_uint_tostring_every_digit_count ()
	{
		uint value = UId (1);
		string digits = StrId ("1");

		for (int n = 1; n <= 10; n++) {
			if (value.ToString () != digits)
				return n + 100;
			if (n == 10)
				break;
			value = value * 10 + (uint) n;
			digits = digits + (char) ('0' + n);
		}
		return 1;
	}

	public static int test_1_uint_tostring_at_the_powers_of_ten ()
	{
		uint power = UId (1);

		for (int n = 1; n <= 9; n++) {
			power = power * 10;
			if ((power - 1).ToString ().Length != n)
				return n + 100;
			if (power.ToString ().Length != n + 1)
				return n + 200;
		}
		return 1;
	}

	public static int test_1_uint_tostring_every_single_digit ()
	{
		for (uint i = 0; i < 10; i++) {
			string s = UId (i).ToString ();
			if (s.Length != 1 || s[0] != (char) ('0' + i))
				return (int) i + 100;
		}
		return 1;
	}

	public static int test_1_uint_tostring_crosses_the_six_digit_split ()
	{
		// The digit count divides by 100000 first, so the values on both sides of
		// that split take different arms.
		if (UId (99999).ToString () != StrId ("99999"))
			return 0;
		if (UId (100000).ToString () != StrId ("100000"))
			return 0;

		return UId (999999).ToString () == StrId ("999999") ? 1 : 0;
	}

	public static int test_1_uint_tostring_matches_manual_digits ()
	{
		uint value = UId (1234567890);
		char[] chars = new char[10];

		for (int i = 9; i >= 0; i--) {
			chars[i] = (char) ('0' + (value % 10));
			value /= 10;
		}

		return UId (1234567890).ToString () == new string (chars) ? 1 : 0;
	}

	// Span.Clear over a reference element type.  The clear that writes references
	// goes through the collector so that a card table sees the store.  The other
	// arm is a plain memset, and intrinsics.cs holds the control for it.

	public static int test_1_span_clear_references_at_every_length ()
	{
		for (int len = 1; len <= 24; len++) {
			object[] array = new object[len];
			for (int i = 0; i < len; i++)
				array[i] = new object ();

			new Span<object> (array).Clear ();

			for (int i = 0; i < len; i++) {
				if (array[i] != null)
					return len + 100;
			}
		}
		return 1;
	}

	public static int test_1_span_clear_references_leaves_the_neighbours ()
	{
		string[] array = new string[8];
		for (int i = 0; i < array.Length; i++)
			array[i] = Build (Id (4), i);

		new Span<string> (array).Slice (Id (2), Id (4)).Clear ();

		if (array[Id (1)] == null || array[Id (6)] == null)
			return 0;

		for (int i = 2; i < 6; i++) {
			if (array[i] != null)
				return i + 100;
		}
		return 1;
	}

	public static int test_1_span_clear_a_struct_with_references ()
	{
		StringIntrinsRefPair[] array = new StringIntrinsRefPair[6];
		for (int i = 0; i < array.Length; i++) {
			array[i].A = new object ();
			array[i].B = Build (Id (3), i);
		}

		new Span<StringIntrinsRefPair> (array).Clear ();

		for (int i = 0; i < array.Length; i++) {
			if (array[i].A != null || array[i].B != null)
				return i + 100;
		}
		return 1;
	}

	public static int test_1_span_clear_of_no_elements ()
	{
		object[] array = new object[] { new object () };

		new Span<object> (array).Slice (Id (1)).Clear ();
		return array[0] != null ? 1 : 0;
	}

	public static int test_1_span_clear_survives_a_collection ()
	{
		// A cleared reference slot has to be a slot the collector accepts: a stale
		// pointer left behind here is only found when something walks the array.
		object[] array = new object[16];
		for (int i = 0; i < array.Length; i++)
			array[i] = new object ();

		new Span<object> (array).Clear ();
		GC.Collect ();

		for (int i = 0; i < array.Length; i++) {
			if (array[i] != null)
				return i + 100;
		}
		return 1;
	}
}
