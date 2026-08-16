// The decisions inside interp_handle_intrinsics that intrinsics.cs,
// intrinsics2.cs, mathintrins.cs and arrays.cs leave alone.  Most of these are
// near misses: a call that reaches an arm of that function, fails one of the
// conditions there, and has to leave as an ordinary call.  A near miss is right
// only when the arm refuses it, so the answer says which way the arm went.
//
// The arms this file cannot reach need a corlib method this tree does not
// build.  System.Text.Unicode.Utf16Utility, System.Text.ASCIIUtility,
// System.Number.UInt32ToDecStr, MemoryMarshal.GetArrayDataReference,
// Array.GetElementSize, Array.IsPrimitive, Math.ScaleB, Math.ILogB,
// Math.Log2 and Math.FusedMultiplyAdd are all absent, and JitHelpers.EnumEquals
// and JitHelpers.EnumCompareTo sit behind #if NETCORE.  The mcs shape of
// Enum.HasFlag needs hand-written IL, because csc emits no constrained. prefix
// in front of a call to a method that is not virtual.
//
// Operands come through NoInlining helpers.  The transform folds constants and
// inlines small callees, and a folded operand leaves the arm untested.

using System;
using System.Runtime.CompilerServices;

public class Intrinsics3 {

	[Flags] enum Intrinsics3Bits : int { None = 0, A = 1, B = 2, C = 4 }
	[Flags] enum Intrinsics3Wide : long { None = 0, A = 1, B = 2 }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Id (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static double D (double x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static float F (float x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object IdObj (object x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static T Keep<T> (T x) { return x; }

	static int Ok (bool held) { return held ? 1 : 0; }

	// Enum.HasFlag.  MINT_INTRINS_ENUM_HASFLAG replaces the csc shape, which is
	// box, ldc, box, call.  intrinsics.cs covers the shape and the two calls that
	// carry no box at all.  These are the two shapes in between.

	public static int test_1_hasflag_of_an_enum_typed_reference ()
	{
		// The flag boxes and the receiver does not, so the instruction under the
		// ldc is the load of the receiver rather than a second box.
		Enum v = (Enum) IdObj (Intrinsics3Bits.A | Intrinsics3Bits.B);

		if (!v.HasFlag (Intrinsics3Bits.B))
			return 0;

		return v.HasFlag (Intrinsics3Bits.C) ? 0 : 1;
	}

	public static int test_1_hasflag_of_two_enum_types_throws ()
	{
		// All three instructions of the shape are here, and the two boxed classes
		// differ.  Only the managed Enum.HasFlag throws on that pair, so the
		// ArgumentException says the arm declined.
		try {
			return Keep (Intrinsics3Bits.A).HasFlag (Intrinsics3Wide.A) ? 2 : 3;
		} catch (ArgumentException) {
			return 1;
		}
	}

	// Type comparison.  op_Equality becomes MINT_CEQ_P and op_Inequality does not,
	// so both answers of the second one come out of the corlib body.

	public static int test_2_type_inequality_stays_a_call ()
	{
		object o = IdObj (new object ());
		object s = IdObj ("intrinsics3");

		if (o.GetType () != typeof (object))
			return 0;

		return o.GetType () != s.GetType () ? 2 : 0;
	}

	// The Set method of a rectangular array of references.  A stored value with a
	// class gets MINT_LDELEMA_TC and a type check.  ldnull gives the arm no class,
	// and then the address comes from MINT_LDELEMA and the store has no check.

	public static int test_2_rank2_set_of_null ()
	{
		string[,] a = new string[Id (2), Id (2)];

		a[Id (1), Id (1)] = Keep ("intrinsics3");
		a[Id (0), Id (0)] = null;

		return Ok (a[1, 1] == "intrinsics3") + Ok (a[0, 0] == null);
	}

	// ReadOnlySpan.get_Length.  The offset in the opcode comes from the _length
	// field of ReadOnlySpan`1, which is a different class from the Span`1 that
	// intrinsics.cs measures.

	public static int test_11_readonlyspan_length_over_a_string ()
	{
		ReadOnlySpan<char> r = Keep ("intrinsics3").AsSpan ();
		return r.Length;
	}

	// The Math and MathF names that reach the table and leave it as calls.  The
	// arms read the parameter count and the parameter types as well as the name,
	// so each of these fails a different one of the three.

	public static int test_3_math_log_with_a_base ()
	{
		// Two doubles, so the names the arm asks for are Atan2 and Pow.  Log is a
		// name the one-parameter arm above answers, which makes this the near miss
		// on the count.
		return (int) Math.Round (Math.Log (D (8.0), D (2.0)));
	}

	public static int test_3_math_sign_of_a_double ()
	{
		// One double, and the S names in the unop table are Sin, Sqrt and Sinh.
		int n = 0;

		if (Math.Sign (D (-2.5)) == -1)
			n++;
		if (Math.Sign (D (0.0)) == 0)
			n++;
		if (Math.Sign (D (7.5)) == 1)
			n++;

		return n;
	}

	public static int test_3_mathf_round_to_a_digit_count ()
	{
		// A float and an int is the shape the last arm reads, and the only name it
		// takes there is ScaleB.  Math has no ScaleB here, so nothing else puts a
		// call of that shape in front of the arm.
		return MathF.Round (F (3.14159f), Id (2)) == 3.14f ? 3 : 0;
	}
}
