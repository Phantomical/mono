// The non-math intrinsics: the calls transform.c answers with an opcode of its
// own, and the span and array opcodes beside them.
//
// A method named test_<n>_<what> is a test, and it passes when it returns <n>.
// Indexes and enum values come through NoInlining helpers.  The transform folds
// constants and inlines small callees, and a folded operand leaves the opcode
// untested.

using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

public class IntrinsicsSuite {

	[Flags] enum IntrinsicsU1 : byte { None = 0, A = 1, B = 2, High = 0x80 }
	[Flags] enum IntrinsicsI1 : sbyte { None = 0, A = 1, B = 2, High = -128 }
	[Flags] enum IntrinsicsU2 : ushort { None = 0, A = 1, B = 2, High = 0x8000 }
	[Flags] enum IntrinsicsI2 : short { None = 0, A = 1, B = 2, High = -32768 }
	[Flags] enum IntrinsicsU4 : uint { None = 0, A = 1, B = 2, High = 0x80000000 }
	[Flags] enum IntrinsicsI4 : int { None = 0, A = 1, B = 2, High = int.MinValue }
	[Flags] enum IntrinsicsU8 : ulong { None = 0, A = 1, B = 2, High = 0x8000000000000000 }
	[Flags] enum IntrinsicsI8 : long { None = 0, A = 1, B = 2, High = long.MinValue }

	struct IntrinsicsPair { public int A; public int B; }
	struct IntrinsicsTriple { public byte A, B, C; }

	class IntrinsicsThing { }
	class IntrinsicsDerived : IntrinsicsThing { }

	class IntrinsicsOwnHash {
		public override int GetHashCode () { return 4242; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Id (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object IdObj (object x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Array IdArray (Array x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int[] IdI4Array (int[] x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IntrinsicsU1 IdU1 (IntrinsicsU1 x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IntrinsicsI1 IdI1 (IntrinsicsI1 x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IntrinsicsU2 IdU2 (IntrinsicsU2 x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IntrinsicsI2 IdI2 (IntrinsicsI2 x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IntrinsicsU4 IdU4 (IntrinsicsU4 x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IntrinsicsI4 IdI4 (IntrinsicsI4 x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IntrinsicsU8 IdU8 (IntrinsicsU8 x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static IntrinsicsI8 IdI8 (IntrinsicsI8 x) { return x; }

	// Enum.HasFlag.  A flag written as a literal is what puts the box/ldc/box
	// shape in front of the call that MINT_INTRINS_ENUM_HASFLAG replaces.  A flag
	// held in a local leaves an ordinary call to Enum.HasFlag, which is the other
	// half of the pair.
	//
	// One test for each width the enum can carry, because the opcode reads the
	// operands through the underlying type.  Each test asks a true question and a
	// false one.  A read at the wrong width still answers true when the flag bits
	// sit inside the value, so the false question is what pins the width down.

	public static int test_1_hasflag_u1_high_bit ()
	{
		if (!IdU1 (IntrinsicsU1.High | IntrinsicsU1.A).HasFlag (IntrinsicsU1.High))
			return 0;

		return IdU1 (IntrinsicsU1.A).HasFlag (IntrinsicsU1.High | IntrinsicsU1.A) ? 0 : 1;
	}

	public static int test_1_hasflag_i1_negative ()
	{
		if (!IdI1 (IntrinsicsI1.High | IntrinsicsI1.A).HasFlag (IntrinsicsI1.High))
			return 0;

		return IdI1 (IntrinsicsI1.A).HasFlag (IntrinsicsI1.High | IntrinsicsI1.A) ? 0 : 1;
	}

	public static int test_1_hasflag_u2_high_bit ()
	{
		if (!IdU2 (IntrinsicsU2.High | IntrinsicsU2.A).HasFlag (IntrinsicsU2.High))
			return 0;

		return IdU2 (IntrinsicsU2.A).HasFlag (IntrinsicsU2.High | IntrinsicsU2.A) ? 0 : 1;
	}

	public static int test_1_hasflag_i2_negative ()
	{
		if (!IdI2 (IntrinsicsI2.High | IntrinsicsI2.A).HasFlag (IntrinsicsI2.High))
			return 0;

		return IdI2 (IntrinsicsI2.A).HasFlag (IntrinsicsI2.High | IntrinsicsI2.A) ? 0 : 1;
	}

	public static int test_1_hasflag_u4_high_bit ()
	{
		if (!IdU4 (IntrinsicsU4.High | IntrinsicsU4.A).HasFlag (IntrinsicsU4.High))
			return 0;

		return IdU4 (IntrinsicsU4.A).HasFlag (IntrinsicsU4.High | IntrinsicsU4.A) ? 0 : 1;
	}

	public static int test_1_hasflag_i4_negative ()
	{
		if (!IdI4 (IntrinsicsI4.High | IntrinsicsI4.A).HasFlag (IntrinsicsI4.High))
			return 0;

		return IdI4 (IntrinsicsI4.A).HasFlag (IntrinsicsI4.High | IntrinsicsI4.A) ? 0 : 1;
	}

	public static int test_1_hasflag_u8_high_bit ()
	{
		if (!IdU8 (IntrinsicsU8.High | IntrinsicsU8.A).HasFlag (IntrinsicsU8.High))
			return 0;

		return IdU8 (IntrinsicsU8.A).HasFlag (IntrinsicsU8.High | IntrinsicsU8.A) ? 0 : 1;
	}

	public static int test_1_hasflag_i8_negative ()
	{
		if (!IdI8 (IntrinsicsI8.High | IntrinsicsI8.A).HasFlag (IntrinsicsI8.High))
			return 0;

		return IdI8 (IntrinsicsI8.A).HasFlag (IntrinsicsI8.High | IntrinsicsI8.A) ? 0 : 1;
	}

	public static int test_1_hasflag_partial_match ()
	{
		return IdI4 (IntrinsicsI4.A | IntrinsicsI4.High).HasFlag (IntrinsicsI4.A | IntrinsicsI4.B) ? 0 : 1;
	}

	public static int test_1_hasflag_variable_flag ()
	{
		IntrinsicsI8 value = IdI8 (IntrinsicsI8.A | IntrinsicsI8.B);
		IntrinsicsI8 flag = IdI8 (IntrinsicsI8.B);

		return value.HasFlag (flag) ? 1 : 0;
	}

	public static int test_1_hasflag_other_enum_type_throws ()
	{
		Enum value = (Enum) IdObj (IntrinsicsI4.A);
		Enum flag = (Enum) IdObj (IntrinsicsU1.A);

		try {
			return value.HasFlag (flag) ? 2 : 3;
		} catch (ArgumentException) {
			return 1;
		}
	}

	public static int test_1_hasflag_null_flag_throws ()
	{
		Enum value = (Enum) IdObj (IntrinsicsI4.A);

		try {
			return value.HasFlag (null) ? 2 : 3;
		} catch (ArgumentNullException) {
			return 1;
		}
	}

	// Object.GetType.

	public static int test_1_gettype_of_object ()
	{
		object o = IdObj (new object ());
		return o.GetType () == typeof (object) ? 1 : 0;
	}

	public static int test_1_gettype_of_boxed_valuetype ()
	{
		object o = IdObj (IntrinsicsU1.B);
		return o.GetType () == typeof (IntrinsicsU1) ? 1 : 0;
	}

	public static int test_1_gettype_ignores_the_static_type ()
	{
		IntrinsicsThing t = (IntrinsicsThing) IdObj (new IntrinsicsDerived ());
		return t.GetType () == typeof (IntrinsicsDerived) ? 1 : 0;
	}

	public static int test_1_gettype_is_one_object_per_class ()
	{
		object a = IdObj (new IntrinsicsThing ());
		object b = IdObj (new IntrinsicsThing ());

		return (object) a.GetType () == (object) b.GetType () ? 1 : 0;
	}

	public static int test_1_gettype_of_null_throws ()
	{
		object o = IdObj (null);

		try {
			return o.GetType () == typeof (object) ? 2 : 3;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	// Identity hash codes.

	public static int test_1_hashcode_matches_runtimehelpers ()
	{
		object o = IdObj (new object ());
		return o.GetHashCode () == RuntimeHelpers.GetHashCode (o) ? 1 : 0;
	}

	public static int test_1_hashcode_separates_two_objects ()
	{
		object a = IdObj (new object ());
		object b = IdObj (new object ());

		return RuntimeHelpers.GetHashCode (a) != RuntimeHelpers.GetHashCode (b) ? 1 : 0;
	}

	public static int test_0_hashcode_of_null ()
	{
		return RuntimeHelpers.GetHashCode (IdObj (null));
	}

	public static int test_1_hashcode_ignores_an_override ()
	{
		// The identity hash comes from the object, so the override cannot reach it.
		object o = IdObj (new IntrinsicsOwnHash ());
		return RuntimeHelpers.GetHashCode (o) != o.GetHashCode () ? 1 : 0;
	}

	// Span length and indexing.

	public static int test_5_span_length_from_array ()
	{
		Span<int> span = new Span<int> (IdI4Array (new int[5]));
		return span.Length;
	}

	public static int test_3_span_reads_an_element ()
	{
		Span<int> span = new Span<int> (IdI4Array (new int[] { 1, 2, 3, 4 }));
		return span[Id (2)];
	}

	public static int test_7_span_writes_an_element ()
	{
		int[] array = IdI4Array (new int[4]);
		Span<int> span = new Span<int> (array);

		span[Id (1)] = 7;
		return array[1];
	}

	public static int test_1_span_index_past_the_end_throws ()
	{
		Span<int> span = new Span<int> (IdI4Array (new int[4]));

		try {
			return span[Id (4)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_1_span_negative_index_throws ()
	{
		Span<int> span = new Span<int> (IdI4Array (new int[4]));

		try {
			return span[Id (-1)];
		} catch (IndexOutOfRangeException) {
			return 1;
		}
	}

	public static int test_4_span_slice_moves_the_pointer ()
	{
		Span<int> span = new Span<int> (IdI4Array (new int[] { 1, 2, 3, 4, 5 }));
		Span<int> tail = span.Slice (Id (3));

		return tail[Id (0)];
	}

	public static int test_9_span_of_byte_element_size ()
	{
		byte[] array = new byte[] { 3, 9, 27 };
		Span<byte> span = new Span<byte> (array);

		return span[Id (1)];
	}

	public static int test_66_span_of_char_element_size ()
	{
		char[] array = new char[] { 'A', 'B', 'C' };
		Span<char> span = new Span<char> (array);

		return span[Id (1)];
	}

	public static int test_8_span_of_struct_element_size ()
	{
		IntrinsicsPair[] array = new IntrinsicsPair[3];
		array[2].A = 8;

		Span<IntrinsicsPair> span = new Span<IntrinsicsPair> (array);
		return span[Id (2)].A;
	}

	public static int test_5_span_of_odd_sized_struct ()
	{
		// Three bytes an element, so the index arithmetic cannot be a shift.
		IntrinsicsTriple[] array = new IntrinsicsTriple[4];
		array[3].B = 5;

		Span<IntrinsicsTriple> span = new Span<IntrinsicsTriple> (array);
		return span[Id (3)].B;
	}

	public static int test_2_readonlyspan_reads_an_element ()
	{
		ReadOnlySpan<int> span = new ReadOnlySpan<int> (IdI4Array (new int[] { 1, 2, 3 }));
		return span[Id (1)];
	}

	// Span over a pointer.  The span the intrinsic builds has to be left on the
	// stack: a construction that goes straight into a local is a call to the
	// constructor, and the opcode is only reached through newobj.

	public unsafe static int test_6_span_over_a_pointer ()
	{
		int* buffer = stackalloc int[4];

		buffer[3] = 6;
		return new Span<int> (buffer, Id (4))[Id (3)];
	}

	public unsafe static int test_0_span_over_a_null_pointer_is_empty ()
	{
		return new Span<int> ((void*) null, Id (0)).Length;
	}

	public unsafe static int test_1_span_over_a_pointer_negative_length_throws ()
	{
		int* buffer = stackalloc int[1];

		try {
			return new Span<int> (buffer, Id (-1)).Length;
		} catch (ArgumentOutOfRangeException) {
			return 1;
		}
	}

	public unsafe static int test_1_span_negative_length_names_no_argument ()
	{
		// Span<T> (void*, int) throws through ThrowHelper, which gives the
		// exception no argument name.  MINT_INTRINS_SPAN_CTOR stands in for that
		// constructor, so it has to report the same.
		int* buffer = stackalloc int[1];

		try {
			return new Span<int> (buffer, Id (-1)).Length;
		} catch (ArgumentOutOfRangeException e) {
			return e.ParamName == null ? 1 : 0;
		}
	}

	// Span.Clear.  A span of references runs MINT_INTRINS_CLEAR_WITH_REFERENCES
	// inside the corlib body.  A span of primitives takes the other arm, which is
	// an ordinary call, so it is the control for the pair.

	public static int test_1_span_clear_with_references ()
	{
		object[] array = new object[] { new object (), new object () };
		Span<object> span = new Span<object> (array);

		span.Clear ();
		return array[0] == null && array[1] == null ? 1 : 0;
	}

	public static int test_1_span_clear_without_references ()
	{
		int[] array = IdI4Array (new int[] { 1, 2, 3 });

		new Span<int> (array).Clear ();
		return array[0] == 0 && array[2] == 0 ? 1 : 0;
	}

	public static int test_11_memorymarshal_getreference_reads ()
	{
		int[] array = IdI4Array (new int[] { 11, 12 });
		Span<int> span = new Span<int> (array);

		return MemoryMarshal.GetReference (span);
	}

	// Array.Rank and Array.Length, which the transform answers without a call.

	public static int test_1_array_rank_of_a_vector ()
	{
		return IdArray (new int[4]).Rank;
	}

	public static int test_2_array_rank_of_two_dimensions ()
	{
		return IdArray (new int[2, 3]).Rank;
	}

	public static int test_1_array_rank_of_null_throws ()
	{
		Array array = IdArray (null);

		try {
			return array.Rank;
		} catch (NullReferenceException) {
			return 1;
		}
	}

	public static int test_5_array_length_through_a_base_reference ()
	{
		return IdArray (new int[5]).Length;
	}

	public static int test_6_array_length_of_a_rectangular_array ()
	{
		// The same opcode as a vector: it reads the total element count, which a
		// rank-2 array also carries.
		return IdArray (new int[2, 3]).Length;
	}

	public static int test_1_array_length_of_null_throws ()
	{
		Array array = IdArray (null);

		try {
			return array.Length;
		} catch (NullReferenceException) {
			return 1;
		}
	}
}
