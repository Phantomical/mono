// Merge points whose predecessors leave different things at the same stack
// depth.  A C# compiler does not emit these; the interpreter assigns one local
// per stack slot, so the width the merge is written with and the width it is
// read with can disagree.

.assembly extern mscorlib { }

.class public auto ansi Merge extends [mscorlib]System.Object
{
	// Both predecessors leave an int64, so the read gets all eight bytes.
	.method public static int32 test_1_merge_i8_i8 () cil managed
	{
		.maxstack 4
		.locals init (int64 v)
		ldc.i4.0
		brfalse IL_wide

		ldc.i8 0x7FFFFFFF00000000
		br IL_join
	IL_wide:
		ldc.i8 0x7FFFFFFF00000000
	IL_join:
		stloc.0
		ldloc.0
		ldc.i8 0x7FFFFFFF00000000
		beq IL_ok
		ldc.i4.0
		ret
	IL_ok:
		ldc.i4.1
		ret
	}

	// The int64 predecessor comes first, so the merge slot is eight bytes wide,
	// and the int32 predecessor then writes four bytes into it. ECMA-335
	// I.12.3.2.1 does not allow the merge, so what the compare below reads is
	// undefined and the test says nothing about it. What it does say is that the
	// interpreter reaches the end: constant folding used to assert on the width
	// it did not expect and take the process with it.
	.method public static int32 test_1_merge_i4_into_i8 () cil managed
	{
		.maxstack 4
		.locals init (int32 taken, int32 narrow)
		ldc.i4.1
		stloc.0
		ldc.i4 5
		stloc.1

		ldloc.0
		brtrue IL_narrow

		ldc.i8 0x7FFFFFFF00000000
		br IL_join
	IL_narrow:
		ldloc.1
	IL_join:
		ldc.i8 5
		bge.un IL_either
	IL_either:
		ldc.i4.1
		ret
	}

	// A branch whose two operands have different stack types. The transform has
	// a path for this that warns and picks one width; what the test says is that
	// it reaches the end rather than asserting.
	.method public static int32 test_1_branch_type_mismatch () cil managed
	{
		.maxstack 4
		.locals init (int32 narrow, int64 wide)
		call int32 Merge::Opaque4 ()
		stloc.0
		call int64 Merge::Opaque8 ()
		stloc.1

		ldloc.0
		ldloc.1
		beq IL_either
	IL_either:
		ldc.i4.1
		ret
	}

	.method public static int32 Opaque4 () cil managed noinlining
	{
		.maxstack 1
		ldc.i4.5
		ret
	}

	.method public static int64 Opaque8 () cil managed noinlining
	{
		.maxstack 1
		ldc.i8 5
		ret
	}
}
