// The comparison opcodes and the conditional branches: ceq, cgt, clt with their
// unsigned forms, and beq, bne.un, bge, bgt, ble, blt with theirs.
//
// The C# compiler picks the shape. A comparison inside a ternary becomes a
// branch opcode, and the same comparison stored into a bool becomes a compare
// opcode, so both shapes are here. Every operand arrives from a NoInlining
// helper, or the transform folds the answer and the opcode never runs.
//
// Each test answers one bit per case, and it covers the true answer and the
// false answer of its opcode. An opcode stuck at one answer fails, and so does
// one that reads > as >=.
//
// NaN is what separates the two families of float compare. An unordered compare
// is false for cgt, clt, bgt and blt, and true for their .un forms.

using System;
using System.Runtime.CompilerServices;

[NoOpt]
public class Comparisons {

	[MethodImpl (MethodImplOptions.NoInlining)] static int I4 (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long I8 (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static float R4 (float x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static double R8 (double x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static object Ref (object x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static sbyte SByte (sbyte x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static byte Byte (byte x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static short Short (short x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static ushort UShort (ushort x) { return x; }

	// ------------------------------------------------------------------
	// ceq
	// ------------------------------------------------------------------

	public static int test_1_ceq_i4 ()
	{
		int a = I4 (7), b = I4 (7), c = I4 (8);
		bool same = a == b, other = a == c;

		return (same ? 1 : 0) | (other ? 2 : 0);
	}

	public static int test_1_ceq_i8 ()
	{
		// The two unequal values differ in one half each, so a compare of
		// four bytes answers one of them wrongly.
		long a = I8 (0x100000000L), b = I8 (0x100000000L);
		long high = I8 (0L), low = I8 (0x100000001L);
		bool same = a == b, other_high = a == high, other_low = a == low;

		return (same ? 1 : 0) | (other_high ? 2 : 0) | (other_low ? 4 : 0);
	}

	public static int test_1_ceq_r8 ()
	{
		double a = R8 (1.5), b = R8 (1.5), c = R8 (2.5);
		bool same = a == b, other = a == c;

		return (same ? 1 : 0) | (other ? 2 : 0);
	}

	public static int test_1_ceq_r4 ()
	{
		float a = R4 (1.5f), b = R4 (1.5f), c = R4 (2.5f);
		bool same = a == b, other = a == c;

		return (same ? 1 : 0) | (other ? 2 : 0);
	}

	public static int test_0_ceq_r8_nan ()
	{
		double nan = R8 (double.NaN), also_nan = R8 (double.NaN), one = R8 (1.0);
		bool both = nan == also_nan, first = nan == one, second = one == nan;

		return (both ? 1 : 0) | (first ? 2 : 0) | (second ? 4 : 0);
	}

	public static int test_0_ceq_r4_nan ()
	{
		float nan = R4 (float.NaN), also_nan = R4 (float.NaN), one = R4 (1.0f);
		bool both = nan == also_nan, first = nan == one, second = one == nan;

		return (both ? 1 : 0) | (first ? 2 : 0) | (second ? 4 : 0);
	}

	public static int test_1_ceq_r8_signed_zeroes ()
	{
		// The two zeroes have different bits and compare equal.
		double a = R8 (0.0), b = R8 (-0.0);
		bool same = a == b;

		return same ? 1 : 0;
	}

	public static int test_1_ceq_ref ()
	{
		object one = Ref (new object ());
		object alias = Ref (one);
		object other = Ref (new object ());
		bool same = one == alias, different = one == other;

		return (same ? 1 : 0) | (different ? 2 : 0);
	}

	public static int test_1_ceq0_i4 ()
	{
		// `x == 0` has an opcode of its own, taken from ldc.i4.0 plus ceq.
		int zero = I4 (0), five = I4 (5);
		bool a = zero == 0, b = five == 0;

		return (a ? 1 : 0) | (b ? 2 : 0);
	}

	// ------------------------------------------------------------------
	// cgt and clt
	// ------------------------------------------------------------------
	// The second and third case of each test are an equal pair and the pair
	// the other way round, which is what a strict compare must answer false.

	public static int test_1_cgt_i4 ()
	{
		int a = I4 (-1), b = I4 (-2), c = I4 (-1);
		bool above = a > b, equal = a > c, below = b > a;

		return (above ? 1 : 0) | (equal ? 2 : 0) | (below ? 4 : 0);
	}

	public static int test_1_cgt_i8 ()
	{
		long a = I8 (-1L), b = I8 (long.MinValue), c = I8 (-1L);
		bool above = a > b, equal = a > c, below = b > a;

		return (above ? 1 : 0) | (equal ? 2 : 0) | (below ? 4 : 0);
	}

	public static int test_1_cgt_r4 ()
	{
		float a = R4 (2.5f), b = R4 (-2.5f), c = R4 (2.5f);
		bool above = a > b, equal = a > c, below = b > a;

		return (above ? 1 : 0) | (equal ? 2 : 0) | (below ? 4 : 0);
	}

	public static int test_1_cgt_r8 ()
	{
		double a = R8 (1e300), b = R8 (-1e300), c = R8 (1e300);
		bool above = a > b, equal = a > c, below = b > a;

		return (above ? 1 : 0) | (equal ? 2 : 0) | (below ? 4 : 0);
	}

	public static int test_0_cgt_r8_nan ()
	{
		double nan = R8 (double.NaN), one = R8 (1.0), also_nan = R8 (double.NaN);
		bool above = nan > one, below = one > nan, both = nan > also_nan;

		return (above ? 1 : 0) | (below ? 2 : 0) | (both ? 4 : 0);
	}

	public static int test_1_clt_i4 ()
	{
		int a = I4 (int.MinValue), b = I4 (0), c = I4 (int.MinValue);
		bool below = a < b, equal = a < c, above = b < a;

		return (below ? 1 : 0) | (equal ? 2 : 0) | (above ? 4 : 0);
	}

	public static int test_1_clt_i8 ()
	{
		long a = I8 (-3L), b = I8 (0x100000000L), c = I8 (-3L);
		bool below = a < b, equal = a < c, above = b < a;

		return (below ? 1 : 0) | (equal ? 2 : 0) | (above ? 4 : 0);
	}

	public static int test_1_clt_r8 ()
	{
		double a = R8 (double.NegativeInfinity), b = R8 (-1e300);
		double c = R8 (double.NegativeInfinity);
		bool below = a < b, equal = a < c, above = b < a;

		return (below ? 1 : 0) | (equal ? 2 : 0) | (above ? 4 : 0);
	}

	public static int test_1_clt_r4 ()
	{
		float a = R4 (-1.5f), b = R4 (0.5f), c = R4 (-1.5f);
		bool below = a < b, equal = a < c, above = b < a;

		return (below ? 1 : 0) | (equal ? 2 : 0) | (above ? 4 : 0);
	}

	public static int test_0_clt_r4_nan ()
	{
		float nan = R4 (float.NaN), one = R4 (1.0f), also_nan = R4 (float.NaN);
		bool below = nan < one, above = one < nan, both = nan < also_nan;

		return (below ? 1 : 0) | (above ? 2 : 0) | (both ? 4 : 0);
	}

	// ------------------------------------------------------------------
	// cgt.un and clt.un
	// ------------------------------------------------------------------

	public static int test_3_cgt_un_clt_un_i4 ()
	{
		// Signed, -1 is below 1. Unsigned, it is the largest value there is.
		uint big = (uint) I4 (-1), one = (uint) I4 (1), also_one = (uint) I4 (1);
		bool above = big > one, below = one < big;
		bool equal_above = one > also_one, reversed_below = big < one;

		return (above ? 1 : 0) | (below ? 2 : 0)
		     | (equal_above ? 4 : 0) | (reversed_below ? 8 : 0);
	}

	public static int test_3_cgt_un_clt_un_i8 ()
	{
		ulong big = (ulong) I8 (-1L), one = (ulong) I8 (1L), also_one = (ulong) I8 (1L);
		bool above = big > one, below = one < big;
		bool equal_above = one > also_one, reversed_below = big < one;

		return (above ? 1 : 0) | (below ? 2 : 0)
		     | (equal_above ? 4 : 0) | (reversed_below ? 8 : 0);
	}

	public static int test_15_cgt_un_clt_un_r8 ()
	{
		// `!(a <= b)` is cgt.un and `!(a >= b)` is clt.un. An unordered pair
		// answers true to both, and an ordered pair answers as cgt and clt do.
		double nan = R8 (double.NaN), one = R8 (1.0), two = R8 (2.0);
		bool unordered_above = !(nan <= one), unordered_below = !(nan >= one);
		bool above = !(two <= one), below = !(one >= two);
		bool not_above = !(one <= two), not_below = !(two >= one);

		return (unordered_above ? 1 : 0) | (unordered_below ? 2 : 0)
		     | (above ? 4 : 0) | (below ? 8 : 0)
		     | (not_above ? 16 : 0) | (not_below ? 32 : 0);
	}

	public static int test_15_cgt_un_clt_un_r4 ()
	{
		float nan = R4 (float.NaN), one = R4 (1.0f), two = R4 (2.0f);
		bool unordered_above = !(nan <= one), unordered_below = !(nan >= one);
		bool above = !(two <= one), below = !(one >= two);
		bool not_above = !(one <= two), not_below = !(two >= one);

		return (unordered_above ? 1 : 0) | (unordered_below ? 2 : 0)
		     | (above ? 4 : 0) | (below ? 8 : 0)
		     | (not_above ? 16 : 0) | (not_below ? 32 : 0);
	}

	public static int test_1_cgt_un_ref ()
	{
		// A reference tested against null compares as an unsigned integer.
		object something = Ref (new object ());
		object nothing = Ref (null);
		bool a = something != null, b = nothing != null;

		return (a ? 1 : 0) | (b ? 2 : 0);
	}

	// ------------------------------------------------------------------
	// The compares the compiler builds out of two opcodes
	// ------------------------------------------------------------------

	public static int test_3_cge_i4 ()
	{
		// `a >= b` is clt followed by the compare against zero, so the equal
		// pair answers true.
		int a = I4 (2), b = I4 (1), c = I4 (2);
		bool above = a >= b, equal = a >= c, below = b >= a;

		return (above ? 1 : 0) | (equal ? 2 : 0) | (below ? 4 : 0);
	}

	public static int test_2_cle_r8 ()
	{
		// `a <= b` is cgt.un followed by the compare against zero, so an
		// unordered pair answers false.
		double nan = R8 (double.NaN), one = R8 (1.0), two = R8 (2.0);
		bool unordered = nan <= one, below = one <= two, above = two <= one;

		return (unordered ? 1 : 0) | (below ? 2 : 0) | (above ? 4 : 0);
	}

	// ------------------------------------------------------------------
	// Operands that are not four bytes wide
	// ------------------------------------------------------------------

	public static int test_15_narrow_operands ()
	{
		// A narrow value has to reach the compare fully extended into the four
		// byte slot the opcode reads.
		sbyte a = SByte (-1);
		byte b = Byte (255);
		short c = Short (-1);
		ushort d = UShort (65535);

		// The last two pairs hold the same bits in the narrow width. A wrong
		// extension makes them compare equal.
		return (a == -1 ? 1 : 0) | (b == 255 ? 2 : 0)
		     | (c == -1 ? 4 : 0) | (d == 65535 ? 8 : 0)
		     | (a == b ? 16 : 0) | (c == d ? 32 : 0);
	}

	public static unsafe int test_3_pointer_compare ()
	{
		int[] cells = new int [2];

		fixed (int* first = cells) {
			int* second = first + 1;
			bool same = first + 1 == second, other = first == second;
			bool below = first < second, above = second < first;

			return (same ? 1 : 0) | (below ? 2 : 0)
			     | (other ? 4 : 0) | (above ? 8 : 0);
		}
	}

	// ------------------------------------------------------------------
	// Conditional branches
	// ------------------------------------------------------------------
	// Each helper answers one bit for each branch it takes. The three calls
	// under it are an above pair, an equal pair and a below pair, so every
	// opcode is seen taken and not taken.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int OrderedI4 (int a, int b)
	{
		return (a >= b ? 1 : 0) | (a > b ? 2 : 0) | (a < b ? 4 : 0) | (a <= b ? 8 : 0);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int OrderedI8 (long a, long b)
	{
		return (a >= b ? 1 : 0) | (a > b ? 2 : 0) | (a < b ? 4 : 0) | (a <= b ? 8 : 0);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int OrderedR8 (double a, double b)
	{
		return (a >= b ? 1 : 0) | (a > b ? 2 : 0) | (a < b ? 4 : 0) | (a <= b ? 8 : 0);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int OrderedR4 (float a, float b)
	{
		return (a >= b ? 1 : 0) | (a > b ? 2 : 0) | (a < b ? 4 : 0) | (a <= b ? 8 : 0);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UnsignedI4 (uint a, uint b)
	{
		return (a >= b ? 1 : 0) | (a > b ? 2 : 0) | (a < b ? 4 : 0) | (a <= b ? 8 : 0);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UnsignedI8 (ulong a, ulong b)
	{
		return (a >= b ? 1 : 0) | (a > b ? 2 : 0) | (a < b ? 4 : 0) | (a <= b ? 8 : 0);
	}

	// The negated comparisons are the .un branches: bge.un, bgt.un, ble.un, blt.un.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UnorderedR8 (double a, double b)
	{
		return (!(a < b) ? 1 : 0) | (!(a <= b) ? 2 : 0)
		     | (!(a > b) ? 4 : 0) | (!(a >= b) ? 8 : 0);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int UnorderedR4 (float a, float b)
	{
		return (!(a < b) ? 1 : 0) | (!(a <= b) ? 2 : 0)
		     | (!(a > b) ? 4 : 0) | (!(a >= b) ? 8 : 0);
	}

	// Above, equal and below give 3, 9 and 12.
	public static int test_24_ordered_branches_i4 ()
	{
		return OrderedI4 (I4 (2), I4 (1)) + OrderedI4 (I4 (1), I4 (1))
		     + OrderedI4 (I4 (1), I4 (2));
	}

	public static int test_24_ordered_branches_i8 ()
	{
		return OrderedI8 (I8 (2L), I8 (1L)) + OrderedI8 (I8 (1L), I8 (1L))
		     + OrderedI8 (I8 (1L), I8 (2L));
	}

	public static int test_24_ordered_branches_r8 ()
	{
		return OrderedR8 (R8 (2.0), R8 (1.0)) + OrderedR8 (R8 (1.0), R8 (1.0))
		     + OrderedR8 (R8 (1.0), R8 (2.0));
	}

	public static int test_24_ordered_branches_r4 ()
	{
		return OrderedR4 (R4 (2.0f), R4 (1.0f)) + OrderedR4 (R4 (1.0f), R4 (1.0f))
		     + OrderedR4 (R4 (1.0f), R4 (2.0f));
	}

	public static int test_0_ordered_branches_r8_nan ()
	{
		return OrderedR8 (R8 (double.NaN), R8 (1.0))
		     + OrderedR8 (R8 (1.0), R8 (double.NaN))
		     + OrderedR8 (R8 (double.NaN), R8 (double.NaN));
	}

	public static int test_0_ordered_branches_r4_nan ()
	{
		return OrderedR4 (R4 (float.NaN), R4 (1.0f))
		     + OrderedR4 (R4 (1.0f), R4 (float.NaN))
		     + OrderedR4 (R4 (float.NaN), R4 (float.NaN));
	}

	// An unordered pair takes all four branches, which is 15. The ordered pairs
	// add 3 and 12.
	public static int test_30_unordered_branches_r8 ()
	{
		return UnorderedR8 (R8 (double.NaN), R8 (1.0))
		     + UnorderedR8 (R8 (2.0), R8 (1.0))
		     + UnorderedR8 (R8 (1.0), R8 (2.0));
	}

	public static int test_30_unordered_branches_r4 ()
	{
		return UnorderedR4 (R4 (float.NaN), R4 (1.0f))
		     + UnorderedR4 (R4 (2.0f), R4 (1.0f))
		     + UnorderedR4 (R4 (1.0f), R4 (2.0f));
	}

	// Unsigned, -1 is the largest value there is, so the first pair answers the
	// opposite of the signed compare.
	public static int test_24_unsigned_branches_i4 ()
	{
		return UnsignedI4 ((uint) I4 (-1), (uint) I4 (1))
		     + UnsignedI4 ((uint) I4 (5), (uint) I4 (5))
		     + UnsignedI4 ((uint) I4 (1), (uint) I4 (-1));
	}

	public static int test_24_unsigned_branches_i8 ()
	{
		return UnsignedI8 ((ulong) I8 (-1L), (ulong) I8 (1L))
		     + UnsignedI8 ((ulong) I8 (5L), (ulong) I8 (5L))
		     + UnsignedI8 ((ulong) I8 (1L), (ulong) I8 (-1L));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int EqualI4 (int a, int b) { return (a == b ? 1 : 0) | (a != b ? 2 : 0); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int EqualI8 (long a, long b) { return (a == b ? 1 : 0) | (a != b ? 2 : 0); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int EqualR8 (double a, double b) { return (a == b ? 1 : 0) | (a != b ? 2 : 0); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int EqualR4 (float a, float b) { return (a == b ? 1 : 0) | (a != b ? 2 : 0); }

	public static int test_3_beq_bne_un_i4 ()
	{
		return EqualI4 (I4 (3), I4 (3)) + EqualI4 (I4 (3), I4 (4));
	}

	public static int test_3_beq_bne_un_i8 ()
	{
		return EqualI8 (I8 (3L), I8 (3L)) + EqualI8 (I8 (3L), I8 (1L << 40));
	}

	public static int test_3_beq_bne_un_r8 ()
	{
		return EqualR8 (R8 (3.0), R8 (3.0)) + EqualR8 (R8 (3.0), R8 (4.0));
	}

	// beq does not branch on an unordered pair and bne.un does, so the answer is 2.
	public static int test_2_beq_bne_un_r8_nan ()
	{
		return EqualR8 (R8 (double.NaN), R8 (double.NaN));
	}

	public static int test_2_beq_bne_un_r4_nan ()
	{
		return EqualR4 (R4 (float.NaN), R4 (1.0f));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int TruthI4 (int a) { return (a != 0 ? 1 : 0) | (a == 0 ? 2 : 0); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int TruthI8 (long a) { return (a != 0 ? 1 : 0) | (a == 0 ? 2 : 0); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int TruthRef (object a) { return (a != null ? 1 : 0) | (a == null ? 2 : 0); }

	public static int test_3_brtrue_brfalse_i4 ()
	{
		return TruthI4 (I4 (5)) + TruthI4 (I4 (0));
	}

	// The high half alone must still read as true.
	public static int test_3_brtrue_brfalse_i8 ()
	{
		return TruthI8 (I8 (1L << 40)) + TruthI8 (I8 (0L));
	}

	public static int test_3_brtrue_brfalse_ref ()
	{
		return TruthRef (Ref (new object ())) + TruthRef (Ref (null));
	}
}
