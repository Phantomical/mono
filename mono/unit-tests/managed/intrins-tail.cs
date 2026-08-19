// The constrained. shape of Enum.HasFlag, and the near misses around it.
//
// interp_handle_intrinsics has two shapes for Enum.HasFlag.  intrinsics.cs and
// valuetypes.cs reach the first one, which is box, ldc, box, call.  The second
// one is ldc, box, constrained., callvirt, and csc does emit it: a receiver
// whose type is a type parameter gets a constrained. prefix, because the
// compiler cannot know that the type is a value type.  A generic method with a
// "struct, Enum" constraint is all this shape needs from C#.
//
// The rewrite turns the box of the flag into a move and puts an ldind in front
// of the receiver.  The underlying type of the enum picks the ldind, so there
// is one test for each opcode interp_get_ldind_for_mt returns here.  uint goes
// with int and ulong with long, because mint_type puts each pair on one type.
//
// Only the four-byte and the eight-byte case put the width into the answer.  A
// narrower flag reaches enum_hasflag through a stackval that truncates it
// again, so those tests cover the opcode alone.
//
// mono/interp/opcodes/intrins.cpp holds handlers this build cannot reach.
// intrinsics3.cs names the corlib methods that are absent.  Three more sit
// behind #ifdef ENABLE_NETCORE, which config.h leaves undefined:
// UNSAFE_ADD_BYTE_OFFSET, UNSAFE_BYTE_OFFSET and
// RUNTIMEHELPERS_OBJECT_HAS_COMPONENT_SIZE.
//
// Operands come through NoInlining helpers.  The transform folds constants and
// inlines small callees, and a folded operand leaves the arm untested.

using System;
using System.Runtime.CompilerServices;

[Instrumented]
public class IntrinsTail {

	[Flags] enum IntrinsTailI1 : sbyte  { None = 0, A = 1, B = 2, High = unchecked ((sbyte) 0x80) }
	[Flags] enum IntrinsTailU1 : byte   { None = 0, A = 1, B = 2, High = 0x80 }
	[Flags] enum IntrinsTailI2 : short  { None = 0, A = 1, B = 2, High = unchecked ((short) 0x8000) }
	[Flags] enum IntrinsTailU2 : ushort { None = 0, A = 1, B = 2, High = 0x8000 }
	[Flags] enum IntrinsTailI4 : int    { None = 0, A = 1, B = 2, High = 0x40000000 }
	[Flags] enum IntrinsTailI8 : long   { None = 0, A = 1, B = 2, High = 0x4000000000000000 }

	// A second int enum, for the arm's test that the boxed flag and the
	// constrained class are the same class.
	[Flags] enum IntrinsTailOther : int { None = 0, A = 1 }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static T Keep<T> (T x) { return x; }

	// One helper per flag constant.  The constant names one enum, so a helper is
	// generic and still usable at one instantiation only.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HighI1<T> (T v) where T : struct, Enum { return v.HasFlag (IntrinsTailI1.High); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HighU1<T> (T v) where T : struct, Enum { return v.HasFlag (IntrinsTailU1.High); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HighI2<T> (T v) where T : struct, Enum { return v.HasFlag (IntrinsTailI2.High); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HighU2<T> (T v) where T : struct, Enum { return v.HasFlag (IntrinsTailU2.High); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HighI4<T> (T v) where T : struct, Enum { return v.HasFlag (IntrinsTailI4.High); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HighI8<T> (T v) where T : struct, Enum { return v.HasFlag (IntrinsTailI8.High); }

	// The six ldind opcodes.  Each flag sits as high in its type as the type has
	// room for, so a read that is too narrow loses it.

	public static int test_1_constrained_hasflag_i1 ()
	{
		if (!HighI1 (Keep (IntrinsTailI1.High | IntrinsTailI1.A)))
			return 0;

		return HighI1 (Keep (IntrinsTailI1.A)) ? 0 : 1;
	}

	public static int test_1_constrained_hasflag_u1 ()
	{
		if (!HighU1 (Keep (IntrinsTailU1.High | IntrinsTailU1.A)))
			return 0;

		return HighU1 (Keep (IntrinsTailU1.A)) ? 0 : 1;
	}

	public static int test_1_constrained_hasflag_i2 ()
	{
		if (!HighI2 (Keep (IntrinsTailI2.High | IntrinsTailI2.A)))
			return 0;

		return HighI2 (Keep (IntrinsTailI2.A)) ? 0 : 1;
	}

	public static int test_1_constrained_hasflag_u2 ()
	{
		if (!HighU2 (Keep (IntrinsTailU2.High | IntrinsTailU2.A)))
			return 0;

		return HighU2 (Keep (IntrinsTailU2.A)) ? 0 : 1;
	}

	public static int test_1_constrained_hasflag_i4 ()
	{
		if (!HighI4 (Keep (IntrinsTailI4.High | IntrinsTailI4.A)))
			return 0;

		return HighI4 (Keep (IntrinsTailI4.A)) ? 0 : 1;
	}

	public static int test_1_constrained_hasflag_i8 ()
	{
		if (!HighI8 (Keep (IntrinsTailI8.High | IntrinsTailI8.A)))
			return 0;

		return HighI8 (Keep (IntrinsTailI8.A)) ? 0 : 1;
	}

	// A receiver that is already a byref.  The rewrite inserts its ldind after
	// the move of an argument rather than after the address of one.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool HighRefI4<T> (ref T v) where T : struct, Enum
	{
		return v.HasFlag (IntrinsTailI4.High);
	}

	public static int test_1_constrained_hasflag_byref_receiver ()
	{
		IntrinsTailI4 held = Keep (IntrinsTailI4.High | IntrinsTailI4.B);
		IntrinsTailI4 missing = Keep (IntrinsTailI4.B);

		if (!HighRefI4 (ref held))
			return 0;

		return HighRefI4 (ref missing) ? 0 : 1;
	}

	// A receiver that is the address of a field of a generic type, which is the
	// third instruction the ldind can land after.

	class IntrinsTailHolder<T> where T : struct, Enum {
		public T Value;

		[MethodImpl (MethodImplOptions.NoInlining)]
		public bool High () { return Value.HasFlag (IntrinsTailI4.High); }
	}

	public static int test_1_constrained_hasflag_field_receiver ()
	{
		var holder = new IntrinsTailHolder<IntrinsTailI4> ();

		holder.Value = Keep (IntrinsTailI4.High | IntrinsTailI4.A);
		if (!holder.High ())
			return 0;

		holder.Value = Keep (IntrinsTailI4.A);
		return holder.High () ? 0 : 1;
	}

	// Two sites in one method.  The second rewrite runs with the instructions of
	// the first already replaced.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int TwoSites<T> (T v) where T : struct, Enum
	{
		int r = 0;

		if (v.HasFlag (IntrinsTailI4.A))
			r |= 1;
		if (v.HasFlag (IntrinsTailI4.High))
			r |= 2;

		return r;
	}

	public static int test_3_constrained_hasflag_two_sites ()
	{
		return TwoSites (Keep (IntrinsTailI4.High | IntrinsTailI4.A));
	}

	// The near misses.  Each one reaches the arm, fails one of its conditions and
	// leaves as an ordinary call to Enum.HasFlag.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool AI4<T> (T v) where T : struct, Enum { return v.HasFlag (IntrinsTailI4.A); }

	public static int test_1_constrained_hasflag_other_class_throws ()
	{
		// The flag and the constrained class are different enums, which is the one
		// condition no other shape of the call can fail.  Only the managed
		// Enum.HasFlag throws on that pair, so the ArgumentException says the arm
		// declined.
		try {
			return AI4 (Keep (IntrinsTailOther.A)) ? 2 : 3;
		} catch (ArgumentException) {
			return 1;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static bool VarFlag<T> (T v, Enum flag) where T : struct, Enum { return v.HasFlag (flag); }

	public static int test_1_constrained_hasflag_variable_flag ()
	{
		// The flag arrives in an argument, so there is no box under the call and
		// the arm cannot read a class off the stack.
		if (!VarFlag (Keep (IntrinsTailI4.High | IntrinsTailI4.A), IntrinsTailI4.High))
			return 0;

		return VarFlag (Keep (IntrinsTailI4.A), IntrinsTailI4.High) ? 0 : 1;
	}
}
