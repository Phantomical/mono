// The constant folder: interp_fold_unop (), interp_fold_binop () and the two
// conditional branch folders in transform.c.
//
// A row in those tables runs only when cprop already knows the operands as
// constants, so every operand here is a literal.  A value test returns what the
// folder computed.  A branch test runs the other path and returns 0 when the
// folder branches the wrong way.
//
// The int64 tests compare the whole result instead of truncating it with
// conv.i4, and each operand pair is chosen so that 32-bit arithmetic answers
// differently.  A row that names the I4 field of the union computes the low
// word and leaves the high word alone.  A truncating test with narrow operands
// sees neither half of that.
//
// MINT_ADD1_I8, MINT_SUB1_I8, MINT_CNE_I8, MINT_CGE_I8, MINT_CGE_UN_I8,
// MINT_CLE_I8 and MINT_CLE_UN_I8 have no other way in.  They come from the nint
// and nuint operators, which interp_handle_magic_type_intrinsics () replaces
// with those opcodes.  il/magic-conv.il documents what makes a type magic.  This
// file needs two things from that rule: one of the assembly names in
// mono_class_is_magic_assembly (), and the two exact type names below.  The
// types hold the members these tests call and nothing more.
//
// The I4 form of cne, cge, cle, cge.un and cle.un never appears on a 64-bit
// target.  Their only producer is the int_cmpop table, which names MINT_*_P,
// and that macro is the I8 opcode when a pointer is eight bytes wide.

.assembly extern mscorlib { }
.assembly 'builtin-types' { }

.namespace System
{

// ---------------------------------------------------------------------------
// nint
// ---------------------------------------------------------------------------
.class public sequential ansi serializable nint
	extends [mscorlib]System.ValueType
{
	.field assembly int64 v

	.method public hidebysig specialname static valuetype System.nint op_Explicit (int64 x) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarg.0
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static int64 op_Implicit (valuetype System.nint x) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Increment (valuetype System.nint x) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldc.i4.1
		conv.i8
		add
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static valuetype System.nint op_Decrement (valuetype System.nint x) cil managed
	{
		.locals init (valuetype System.nint r)
		ldloca.s 0
		ldarga.s 0
		ldfld int64 System.nint::v
		ldc.i4.1
		conv.i8
		sub
		stfld int64 System.nint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static bool op_Inequality (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		ceq
		ldc.i4.0
		ceq
		ret
	}

	.method public hidebysig specialname static bool op_LessThanOrEqual (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		cgt
		ldc.i4.0
		ceq
		ret
	}

	.method public hidebysig specialname static bool op_GreaterThanOrEqual (valuetype System.nint a, valuetype System.nint b) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		ldarga.s 1
		ldfld int64 System.nint::v
		clt
		ldc.i4.0
		ceq
		ret
	}
}

// ---------------------------------------------------------------------------
// nuint
// ---------------------------------------------------------------------------
.class public sequential ansi serializable nuint
	extends [mscorlib]System.ValueType
{
	.field assembly uint64 v

	.method public hidebysig specialname static valuetype System.nuint op_Explicit (uint64 x) cil managed
	{
		.locals init (valuetype System.nuint r)
		ldloca.s 0
		ldarg.0
		stfld uint64 System.nuint::v
		ldloc.0
		ret
	}

	.method public hidebysig specialname static bool op_LessThanOrEqual (valuetype System.nuint a, valuetype System.nuint b) cil managed
	{
		ldarga.s 0
		ldfld uint64 System.nuint::v
		ldarga.s 1
		ldfld uint64 System.nuint::v
		cgt.un
		ldc.i4.0
		ceq
		ret
	}

	.method public hidebysig specialname static bool op_GreaterThanOrEqual (valuetype System.nuint a, valuetype System.nuint b) cil managed
	{
		ldarga.s 0
		ldfld uint64 System.nuint::v
		ldarga.s 1
		ldfld uint64 System.nuint::v
		clt.un
		ldc.i4.0
		ceq
		ret
	}
}

} // .namespace System

// The attribute cs/arms.cs holds.  This file is an assembly of its own, and the
// runner matches an arm attribute by name, so it carries its own definition
// rather than referencing that one.
.class public auto ansi sealed NoOptAttribute extends [mscorlib]System.Attribute
{
	.method public specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Attribute::.ctor ()
		ret
	}
}

// ---------------------------------------------------------------------------
// The tests
// ---------------------------------------------------------------------------
.class public auto ansi Folding extends [mscorlib]System.Object
{
	// Every operand here is a literal, so the arm that turns the folder off
	// runs each of these rows through the opcode it folds to instead.
	.custom instance void NoOptAttribute::.ctor() = ( 01 00 00 00 )

	// -------------------------------------------------------------------
	// interp_fold_unop.  0xFFFFFFFEFFFFFFF9 is -0x100000007, which has a
	// bit set in each half.
	// -------------------------------------------------------------------
	.method public static int32 test_1_neg_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 0xFFFFFFFEFFFFFFF9
		neg
		ldc.i8 0x100000007
		ceq
		ret
	}

	.method public static int32 test_5_not_i4 () cil managed
	{
		.maxstack 2
		ldc.i4 -6
		not
		ret
	}

	.method public static int32 test_1_not_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 0xFFFFFFFEFFFFFFF9
		not
		ldc.i8 0x100000006
		ceq
		ret
	}

	// -------------------------------------------------------------------
	// interp_fold_unop_cond_br.  Each method takes the branch once and
	// declines it once, which is both arms of the row.  0x100000000 separates
	// the I8 row from the I4 one: true as an int64, false in its low word.
	// -------------------------------------------------------------------
	.method public static int32 test_1_brfalse_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 0
		brfalse.s TAKEN
		ldc.i4.0
		ret
	TAKEN:
		ldc.i8 0x100000000
		brfalse.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public static int32 test_1_brtrue_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 0x100000000
		brtrue.s TAKEN
		ldc.i4.0
		ret
	TAKEN:
		ldc.i8 0
		brtrue.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	// -------------------------------------------------------------------
	// interp_fold_binop: arithmetic, bitwise and shifts
	// -------------------------------------------------------------------
	.method public static int32 test_1_sub_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 0x30000000B
		ldc.i8 0x100000009
		sub
		ldc.i8 0x200000002
		ceq
		ret
	}

	// 8 + 14 + 6 in the low word and 1 + 3 + 2 in the high one.  A wrong
	// answer from any of the three moves the sum.
	.method public static int32 test_1_bitwise_i8 () cil managed
	{
		.maxstack 4
		ldc.i8 0x30000000C
		ldc.i8 0x10000000A
		and
		ldc.i8 0x30000000C
		ldc.i8 0x10000000A
		or
		add
		ldc.i8 0x30000000C
		ldc.i8 0x10000000A
		xor
		add
		ldc.i8 0x60000001C
		ceq
		ret
	}

	// INTERP_FOLD_SHIFTOP reads the count as an int32 whatever width the
	// value has, so all three shift tests push an int32 count.  A count past
	// 32 separates this row from the I4 one.
	.method public static int32 test_1_shl_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 5
		ldc.i4.s 35
		shl
		ldc.i8 0x2800000000
		ceq
		ret
	}

	// -40 >> 3 is -5, done a word up so the sign fills the high half.
	.method public static int32 test_1_shr_i8_is_arithmetic () cil managed
	{
		.maxstack 2
		ldc.i8 0xFFFFFFD800000000
		ldc.i4.3
		shr
		ldc.i8 0xFFFFFFFB00000000
		ceq
		ret
	}

	.method public static int32 test_1_shr_un_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 -1
		ldc.i4.4
		shr.un
		ldc.i8 0x0FFFFFFFFFFFFFFF
		ceq
		ret
	}

	// -------------------------------------------------------------------
	// interp_fold_binop: relops.  Each is a true case less a false case,
	// for the I4 row and then the I8 row.  Each int64 pair orders one way
	// over 64 bits and the other way over its low words.
	// -------------------------------------------------------------------
	.method public static int32 test_1_ceq_i4 () cil managed
	{
		.maxstack 4
		ldc.i4.7
		ldc.i4.7
		ceq
		ldc.i4.7
		ldc.i4.8
		ceq
		sub
		ret
	}

	.method public static int32 test_2_cgt () cil managed
	{
		.maxstack 4
		ldc.i4.5
		ldc.i4.3
		cgt
		ldc.i4.3
		ldc.i4.5
		cgt
		sub
		ldc.i8 0x100000000
		ldc.i8 1
		cgt
		add
		ldc.i8 1
		ldc.i8 0x100000000
		cgt
		sub
		ret
	}

	// -1 and 0x8000000000000001 are the larger operand read unsigned, so a
	// signed fold answers -2.
	.method public static int32 test_2_cgt_un () cil managed
	{
		.maxstack 4
		ldc.i4.m1
		ldc.i4.3
		cgt.un
		ldc.i4.3
		ldc.i4.m1
		cgt.un
		sub
		ldc.i8 0x8000000000000001
		ldc.i8 2
		cgt.un
		add
		ldc.i8 2
		ldc.i8 0x8000000000000001
		cgt.un
		sub
		ret
	}

	.method public static int32 test_2_clt () cil managed
	{
		.maxstack 4
		ldc.i4.m1
		ldc.i4.3
		clt
		ldc.i4.3
		ldc.i4.m1
		clt
		sub
		ldc.i8 1
		ldc.i8 0x100000000
		clt
		add
		ldc.i8 0x100000000
		ldc.i8 1
		clt
		sub
		ret
	}

	.method public static int32 test_2_clt_un () cil managed
	{
		.maxstack 4
		ldc.i4.3
		ldc.i4.m1
		clt.un
		ldc.i4.m1
		ldc.i4.3
		clt.un
		sub
		ldc.i8 2
		ldc.i8 0x8000000000000001
		clt.un
		add
		ldc.i8 0x8000000000000001
		ldc.i8 2
		clt.un
		sub
		ret
	}

	// -------------------------------------------------------------------
	// interp_fold_binop: division and remainder
	// -------------------------------------------------------------------
	.method public static int32 test_2147483647_div_un_i4 () cil managed
	{
		.maxstack 2
		ldc.i4 -2
		ldc.i4.2
		div.un
		ret
	}

	.method public static int32 test_1_div_un_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 -2
		ldc.i8 2
		div.un
		ldc.i8 0x7FFFFFFFFFFFFFFF
		ceq
		ret
	}

	.method public static int32 test_1_rem_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 0x500000011
		ldc.i8 0x200000000
		rem
		ldc.i8 0x100000011
		ceq
		ret
	}

	.method public static int32 test_3_rem_un_i4 () cil managed
	{
		.maxstack 2
		ldc.i4 -3
		ldc.i4.s 10
		rem.un
		ret
	}

	// A divisor of 7 answers 1 over 64 bits, 3 over the low word and -1 read
	// signed.  This pair therefore separates the row from both of the rows
	// beside it.
	.method public static int32 test_1_rem_un_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 -1
		ldc.i8 7
		rem.un
		ldc.i8 1
		ceq
		ret
	}

	// A divisor of zero fails the condition on the fold, so the opcode stays
	// and throws when it runs.
	.method public static int32 test_1_div_un_i4_by_zero_throws () cil managed
	{
		.maxstack 2
		.locals init (int32 quotient, int32 answer)
		.try {
			ldc.i4.8
			ldc.i4.0
			div.un
			stloc.0
			ldc.i4.0
			stloc.1
			leave.s DONE
		} catch [mscorlib]System.DivideByZeroException {
			pop
			ldc.i4.1
			stloc.1
			leave.s DONE
		}
	DONE:
		ldloc.1
		ret
	}

	// The other half of that condition: the one pair a signed remainder
	// cannot compute.
	.method public static int32 test_1_rem_i8_min_by_minus_one_throws () cil managed
	{
		.maxstack 2
		.locals init (int64 rest, int32 answer)
		.try {
			ldc.i8 0x8000000000000000
			ldc.i8 -1
			rem
			stloc.0
			ldc.i4.0
			stloc.1
			leave.s DONE
		} catch [mscorlib]System.OverflowException {
			pop
			ldc.i4.1
			stloc.1
			leave.s DONE
		}
	DONE:
		ldloc.1
		ret
	}

	// -------------------------------------------------------------------
	// interp_fold_binop_cond_br.  Each method takes its branch once and
	// declines it once.  The signed int64 pairs are 0x100000000 against 1.
	// Those two order the other way over their low words.
	// -------------------------------------------------------------------
	.method public static int32 test_1_bge_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 0x100000000
		ldc.i8 1
		bge.s TAKEN
		ldc.i4.0
		ret
	TAKEN:
		ldc.i8 1
		ldc.i8 0x100000000
		bge.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public static int32 test_1_bgt_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 0x100000000
		ldc.i8 1
		bgt.s TAKEN
		ldc.i4.0
		ret
	TAKEN:
		ldc.i8 1
		ldc.i8 0x100000000
		bgt.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public static int32 test_1_ble_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 1
		ldc.i8 0x100000000
		ble.s TAKEN
		ldc.i4.0
		ret
	TAKEN:
		ldc.i8 0x100000000
		ldc.i8 1
		ble.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public static int32 test_1_blt () cil managed
	{
		.maxstack 2
		ldc.i4.3
		ldc.i4.5
		blt.s FOUR
		ldc.i4.0
		ret
	FOUR:
		ldc.i4.5
		ldc.i4.3
		blt.s FAIL
		ldc.i8 1
		ldc.i8 0x100000000
		blt.s EIGHT
		ldc.i4.0
		ret
	EIGHT:
		ldc.i8 0x100000000
		ldc.i8 1
		blt.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	// The unsigned branches take -1 and 0x8000000000000001 as the larger
	// operand.  A signed fold answers 0 on every one of these.  So does a
	// fold that reads the int64 pair 32 bits wide.
	.method public static int32 test_1_bge_un () cil managed
	{
		.maxstack 2
		ldc.i4.m1
		ldc.i4.3
		bge.un.s FOUR
		ldc.i4.0
		ret
	FOUR:
		ldc.i4.3
		ldc.i4.m1
		bge.un.s FAIL
		ldc.i8 0x8000000000000001
		ldc.i8 2
		bge.un.s EIGHT
		ldc.i4.0
		ret
	EIGHT:
		ldc.i8 2
		ldc.i8 0x8000000000000001
		bge.un.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public static int32 test_1_bgt_un () cil managed
	{
		.maxstack 2
		ldc.i4.m1
		ldc.i4.3
		bgt.un.s FOUR
		ldc.i4.0
		ret
	FOUR:
		ldc.i4.3
		ldc.i4.m1
		bgt.un.s FAIL
		ldc.i8 0x8000000000000001
		ldc.i8 2
		bgt.un.s EIGHT
		ldc.i4.0
		ret
	EIGHT:
		ldc.i8 2
		ldc.i8 0x8000000000000001
		bgt.un.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public static int32 test_1_ble_un () cil managed
	{
		.maxstack 2
		ldc.i4.3
		ldc.i4.m1
		ble.un.s FOUR
		ldc.i4.0
		ret
	FOUR:
		ldc.i4.m1
		ldc.i4.3
		ble.un.s FAIL
		ldc.i8 2
		ldc.i8 0x8000000000000001
		ble.un.s EIGHT
		ldc.i4.0
		ret
	EIGHT:
		ldc.i8 0x8000000000000001
		ldc.i8 2
		ble.un.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	.method public static int32 test_1_blt_un () cil managed
	{
		.maxstack 2
		ldc.i4.3
		ldc.i4.m1
		blt.un.s FOUR
		ldc.i4.0
		ret
	FOUR:
		ldc.i4.m1
		ldc.i4.3
		blt.un.s FAIL
		ldc.i8 2
		ldc.i8 0x8000000000000001
		blt.un.s EIGHT
		ldc.i4.0
		ret
	EIGHT:
		ldc.i8 0x8000000000000001
		ldc.i8 2
		blt.un.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	// -------------------------------------------------------------------
	// The rows the nint and nuint operators reach.  op_Explicit and
	// op_Implicit emit no instruction when the source and the target are
	// both eight bytes wide.  The literal therefore reaches the operator as
	// a constant the folder knows, and comes back out as one.
	// -------------------------------------------------------------------
	.method public static int32 test_1_nint_increment () cil managed
	{
		.maxstack 2
		ldc.i8 0x1000000FF
		call valuetype System.nint System.nint::op_Explicit(int64)
		call valuetype System.nint System.nint::op_Increment(valuetype System.nint)
		call int64 System.nint::op_Implicit(valuetype System.nint)
		ldc.i8 0x100000100
		ceq
		ret
	}

	.method public static int32 test_1_nint_decrement () cil managed
	{
		.maxstack 2
		ldc.i8 0x100000100
		call valuetype System.nint System.nint::op_Explicit(int64)
		call valuetype System.nint System.nint::op_Decrement(valuetype System.nint)
		call int64 System.nint::op_Implicit(valuetype System.nint)
		ldc.i8 0x1000000FF
		ceq
		ret
	}

	.method public static int32 test_1_nint_inequality () cil managed
	{
		.maxstack 4
		ldc.i8 0x100000000
		call valuetype System.nint System.nint::op_Explicit(int64)
		ldc.i8 0
		call valuetype System.nint System.nint::op_Explicit(int64)
		call bool System.nint::op_Inequality(valuetype System.nint, valuetype System.nint)
		ldc.i8 0x100000000
		call valuetype System.nint System.nint::op_Explicit(int64)
		ldc.i8 0x100000000
		call valuetype System.nint System.nint::op_Explicit(int64)
		call bool System.nint::op_Inequality(valuetype System.nint, valuetype System.nint)
		sub
		ret
	}

	.method public static int32 test_1_nint_greater_than_or_equal () cil managed
	{
		.maxstack 4
		ldc.i8 0x100000000
		call valuetype System.nint System.nint::op_Explicit(int64)
		ldc.i8 1
		call valuetype System.nint System.nint::op_Explicit(int64)
		call bool System.nint::op_GreaterThanOrEqual(valuetype System.nint, valuetype System.nint)
		ldc.i8 1
		call valuetype System.nint System.nint::op_Explicit(int64)
		ldc.i8 0x100000000
		call valuetype System.nint System.nint::op_Explicit(int64)
		call bool System.nint::op_GreaterThanOrEqual(valuetype System.nint, valuetype System.nint)
		sub
		ret
	}

	.method public static int32 test_1_nint_less_than_or_equal () cil managed
	{
		.maxstack 4
		ldc.i8 1
		call valuetype System.nint System.nint::op_Explicit(int64)
		ldc.i8 0x100000000
		call valuetype System.nint System.nint::op_Explicit(int64)
		call bool System.nint::op_LessThanOrEqual(valuetype System.nint, valuetype System.nint)
		ldc.i8 0x100000000
		call valuetype System.nint System.nint::op_Explicit(int64)
		ldc.i8 1
		call valuetype System.nint System.nint::op_Explicit(int64)
		call bool System.nint::op_LessThanOrEqual(valuetype System.nint, valuetype System.nint)
		sub
		ret
	}

	// The nuint operators pick the unsigned opcode, so the high bit makes the
	// larger operand.  0x8000000000000000 is zero in its low word, so a fold
	// that reads 32 bits also orders these the other way.
	.method public static int32 test_1_nuint_greater_than_or_equal () cil managed
	{
		.maxstack 4
		ldc.i8 0x8000000000000000
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		ldc.i8 1
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		call bool System.nuint::op_GreaterThanOrEqual(valuetype System.nuint, valuetype System.nuint)
		ldc.i8 1
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		ldc.i8 0x8000000000000000
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		call bool System.nuint::op_GreaterThanOrEqual(valuetype System.nuint, valuetype System.nuint)
		sub
		ret
	}

	.method public static int32 test_1_nuint_less_than_or_equal () cil managed
	{
		.maxstack 4
		ldc.i8 1
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		ldc.i8 0x8000000000000000
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		call bool System.nuint::op_LessThanOrEqual(valuetype System.nuint, valuetype System.nuint)
		ldc.i8 0x8000000000000000
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		ldc.i8 1
		call valuetype System.nuint System.nuint::op_Explicit(uint64)
		call bool System.nuint::op_LessThanOrEqual(valuetype System.nuint, valuetype System.nuint)
		sub
		ret
	}
}
