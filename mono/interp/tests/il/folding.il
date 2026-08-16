// The constant folder: interp_fold_unop (), interp_fold_binop () and the two
// conditional branch folders in transform.c.
//
// A row in those tables runs only when cprop already knows both operands as
// constants, so these tests put literals on both sides.  That is the opposite
// of what the other suites do.  Each method returns what the folder computed,
// so a row that folds to the wrong constant fails its test.
//
// Some rows have no other way in.  MINT_ADD1_I8, MINT_SUB1_I8, MINT_CNE_I8,
// MINT_CGE_I8, MINT_CLE_I8 and the two unsigned forms come from the nint and
// nuint operators, which the transform replaces with those opcodes.  A type is
// magic only when it is System.nint or System.nuint and the assembly holding it
// has one of the names in mono_class_is_magic_assembly (), which is why this
// assembly is called builtin-types and why the two types below keep their exact
// names.  They hold the members these tests call and nothing more.
//
// The I4 forms of cne, cge, cle, cge.un and cle.un stay uncovered: their table
// rows name MINT_*_P, which is the I8 opcode on a 64-bit target.

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

	.method public hidebysig specialname static int32 op_Explicit (valuetype System.nint x) cil managed
	{
		ldarga.s 0
		ldfld int64 System.nint::v
		conv.i4
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

// ---------------------------------------------------------------------------
// The tests
// ---------------------------------------------------------------------------
.class public auto ansi Folding extends [mscorlib]System.Object
{
	// -------------------------------------------------------------------
	// interp_fold_unop
	// -------------------------------------------------------------------
	.method public static int32 test_7_neg_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 -7
		neg
		conv.i4
		ret
	}

	.method public static int32 test_5_not_i4 () cil managed
	{
		.maxstack 2
		ldc.i4 -6
		not
		ret
	}

	.method public static int32 test_5_not_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 -6
		not
		conv.i4
		ret
	}

	// -------------------------------------------------------------------
	// interp_fold_unop_cond_br.  Each method takes the branch once and
	// declines it once, which is both arms of the row.
	// -------------------------------------------------------------------
	.method public static int32 test_1_brfalse_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 0
		brfalse.s TAKEN
		ldc.i4.0
		ret
	TAKEN:
		ldc.i8 7
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
		ldc.i8 7
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
	.method public static int32 test_2_sub_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 9
		ldc.i8 7
		sub
		conv.i4
		ret
	}

	// 8 + 14 + 6.  A wrong answer from any of the three moves the sum.
	.method public static int32 test_28_bitwise_i8 () cil managed
	{
		.maxstack 4
		ldc.i8 12
		ldc.i8 10
		and
		ldc.i8 12
		ldc.i8 10
		or
		add
		ldc.i8 12
		ldc.i8 10
		xor
		add
		conv.i4
		ret
	}

	.method public static int32 test_40_shl_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 5
		ldc.i4.3
		shl
		conv.i4
		ret
	}

	// The shift count is an int32 while the value is an int64, which is what
	// INTERP_FOLD_SHIFTOP reads.
	.method public static int32 test_1_shr_i8_is_arithmetic () cil managed
	{
		.maxstack 2
		ldc.i8 -40
		ldc.i4.3
		shr
		ldc.i8 -5
		ceq
		ret
	}

	.method public static int32 test_15_shr_un_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 -1
		ldc.i4.s 60
		shr.un
		conv.i4
		ret
	}

	// -------------------------------------------------------------------
	// interp_fold_binop: relops.  Each is a true case less a false case.
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
		ldc.i8 5
		ldc.i8 3
		cgt
		add
		ldc.i8 3
		ldc.i8 5
		cgt
		sub
		ret
	}

	// -1 is the larger operand read unsigned, so a signed fold answers -2.
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
		ldc.i8 -1
		ldc.i8 3
		cgt.un
		add
		ldc.i8 3
		ldc.i8 -1
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
		ldc.i8 -1
		ldc.i8 3
		clt
		add
		ldc.i8 3
		ldc.i8 -1
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
		ldc.i8 3
		ldc.i8 -1
		clt.un
		add
		ldc.i8 -1
		ldc.i8 3
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

	.method public static int32 test_2_rem_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 17
		ldc.i8 5
		rem
		conv.i4
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

	.method public static int32 test_3_rem_un_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 -3
		ldc.i8 10
		rem.un
		conv.i4
		ret
	}

	// A divisor of zero fails the condition on the fold, so the opcode stays
	// and runs.  The local keeps the quotient live, or deadce would drop the
	// instruction that throws.
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
	// declines it once.
	// -------------------------------------------------------------------
	.method public static int32 test_1_bge_i8 () cil managed
	{
		.maxstack 2
		ldc.i8 5
		ldc.i8 5
		bge.s TAKEN
		ldc.i4.0
		ret
	TAKEN:
		ldc.i8 4
		ldc.i8 5
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
		ldc.i8 6
		ldc.i8 5
		bgt.s TAKEN
		ldc.i4.0
		ret
	TAKEN:
		ldc.i8 5
		ldc.i8 5
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
		ldc.i8 5
		ldc.i8 5
		ble.s TAKEN
		ldc.i4.0
		ret
	TAKEN:
		ldc.i8 6
		ldc.i8 5
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
		ldc.i8 3
		ldc.i8 5
		blt.s EIGHT
		ldc.i4.0
		ret
	EIGHT:
		ldc.i8 5
		ldc.i8 3
		blt.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	// The unsigned branches take -1 as the larger operand.  A signed fold
	// answers 0 on every one of these.
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
		ldc.i8 -1
		ldc.i8 3
		bge.un.s EIGHT
		ldc.i4.0
		ret
	EIGHT:
		ldc.i8 3
		ldc.i8 -1
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
		ldc.i8 -1
		ldc.i8 3
		bgt.un.s EIGHT
		ldc.i4.0
		ret
	EIGHT:
		ldc.i8 3
		ldc.i8 -1
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
		ldc.i8 3
		ldc.i8 -1
		ble.un.s EIGHT
		ldc.i4.0
		ret
	EIGHT:
		ldc.i8 -1
		ldc.i8 3
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
		ldc.i8 3
		ldc.i8 -1
		blt.un.s EIGHT
		ldc.i4.0
		ret
	EIGHT:
		ldc.i8 -1
		ldc.i8 3
		blt.un.s FAIL
		ldc.i4.1
		ret
	FAIL:
		ldc.i4.0
		ret
	}

	// -------------------------------------------------------------------
	// The rows the nint and nuint operators reach.  op_Explicit between two
	// widths of eight emits nothing, so the literal reaches the operator as
	// a constant the folder knows.
	// -------------------------------------------------------------------
	.method public static int32 test_43_nint_increment () cil managed
	{
		.maxstack 2
		ldc.i8 42
		call valuetype System.nint System.nint::op_Explicit(int64)
		call valuetype System.nint System.nint::op_Increment(valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public static int32 test_41_nint_decrement () cil managed
	{
		.maxstack 2
		ldc.i8 42
		call valuetype System.nint System.nint::op_Explicit(int64)
		call valuetype System.nint System.nint::op_Decrement(valuetype System.nint)
		call int32 System.nint::op_Explicit(valuetype System.nint)
		ret
	}

	.method public static int32 test_1_nint_inequality () cil managed
	{
		.maxstack 4
		ldc.i8 4
		call valuetype System.nint System.nint::op_Explicit(int64)
		ldc.i8 5
		call valuetype System.nint System.nint::op_Explicit(int64)
		call bool System.nint::op_Inequality(valuetype System.nint, valuetype System.nint)
		ldc.i8 4
		call valuetype System.nint System.nint::op_Explicit(int64)
		ldc.i8 4
		call valuetype System.nint System.nint::op_Explicit(int64)
		call bool System.nint::op_Inequality(valuetype System.nint, valuetype System.nint)
		sub
		ret
	}

	.method public static int32 test_1_nint_greater_than_or_equal () cil managed
	{
		.maxstack 4
		ldc.i8 5
		call valuetype System.nint System.nint::op_Explicit(int64)
		ldc.i8 5
		call valuetype System.nint System.nint::op_Explicit(int64)
		call bool System.nint::op_GreaterThanOrEqual(valuetype System.nint, valuetype System.nint)
		ldc.i8 4
		call valuetype System.nint System.nint::op_Explicit(int64)
		ldc.i8 5
		call valuetype System.nint System.nint::op_Explicit(int64)
		call bool System.nint::op_GreaterThanOrEqual(valuetype System.nint, valuetype System.nint)
		sub
		ret
	}

	.method public static int32 test_1_nint_less_than_or_equal () cil managed
	{
		.maxstack 4
		ldc.i8 5
		call valuetype System.nint System.nint::op_Explicit(int64)
		ldc.i8 5
		call valuetype System.nint System.nint::op_Explicit(int64)
		call bool System.nint::op_LessThanOrEqual(valuetype System.nint, valuetype System.nint)
		ldc.i8 6
		call valuetype System.nint System.nint::op_Explicit(int64)
		ldc.i8 5
		call valuetype System.nint System.nint::op_Explicit(int64)
		call bool System.nint::op_LessThanOrEqual(valuetype System.nint, valuetype System.nint)
		sub
		ret
	}

	// The nuint operators pick the unsigned opcode, so the high bit makes the
	// larger operand.
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
