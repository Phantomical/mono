// Argument and local addresses, and the return forms that go with a localloc.

.assembly extern mscorlib { }
.module extern 'interp-tests-cs.netmodule'

.class public sequential ansi sealed LocalsPair extends [mscorlib]System.ValueType
{
	.field public int32 first
	.field public int64 second
}

.class public sequential ansi sealed LocalsWide extends [mscorlib]System.ValueType
{
	.field public int64 a
	.field public int64 b
	.field public int64 c
	.field public int64 d
}

.class public auto ansi LocalsIL extends [mscorlib]System.Object
{
	.custom instance void [.module 'interp-tests-cs.netmodule']NoOptAttribute::.ctor() = ( 01 00 00 00 )
	.method public static int32 ReadThrough (int32& p) cil managed noinlining
	{
		.maxstack 1
		ldarg.0
		ldind.i4
		ret
	}

	.method public static int32 test_5_ldarga_of_a_scalar () cil managed
	{
		.maxstack 2
		ldc.i4.5
		call int32 LocalsIL::TakesScalar (int32)
		ret
	}

	.method public static int32 TakesScalar (int32 v) cil managed noinlining
	{
		.maxstack 2
		ldarga.s 0
		call int32 LocalsIL::ReadThrough (int32&)
		ret
	}

	.method public static int32 test_10_ldarga_of_a_struct () cil managed
	{
		.maxstack 3
		.locals init (valuetype LocalsPair p)
		ldloca.s 0
		ldc.i4.4
		stfld int32 LocalsPair::first
		ldloca.s 0
		ldc.i4.5
		conv.i8
		stfld int64 LocalsPair::second
		ldloc.0
		call int32 LocalsIL::TakesStruct (valuetype LocalsPair)
		ret
	}

	.method public static int32 TakesStruct (valuetype LocalsPair p) cil managed noinlining
	{
		.maxstack 3
		ldarga.s 0
		dup
		ldfld int32 LocalsPair::first
		ldc.i4.1
		add
		stfld int32 LocalsPair::first
		ldarga.s 0
		ldfld int32 LocalsPair::first
		ldarga.s 0
		ldfld int64 LocalsPair::second
		conv.i4
		add
		ret
	}

	// A struct wider than a register pair, so the callee works through its
	// address rather than through a copy in a slot.
	.method public static int32 test_10_ldarga_of_a_wide_struct () cil managed
	{
		.maxstack 3
		.locals init (valuetype LocalsWide w)
		ldloca.s 0
		initobj LocalsWide
		ldloca.s 0
		ldc.i4.1
		conv.i8
		stfld int64 LocalsWide::a
		ldloca.s 0
		ldc.i4.2
		conv.i8
		stfld int64 LocalsWide::b
		ldloca.s 0
		ldc.i4.3
		conv.i8
		stfld int64 LocalsWide::c
		ldloca.s 0
		ldc.i4.4
		conv.i8
		stfld int64 LocalsWide::d
		ldloc.0
		call int32 LocalsIL::TakesWide (valuetype LocalsWide)
		ret
	}

	.method public static int32 TakesWide (valuetype LocalsWide w) cil managed noinlining
	{
		.maxstack 3
		ldarga.s 0
		ldfld int64 LocalsWide::a
		ldarga.s 0
		ldfld int64 LocalsWide::b
		add
		ldarga.s 0
		ldfld int64 LocalsWide::c
		add
		ldarga.s 0
		ldfld int64 LocalsWide::d
		add
		conv.i4
		ldc.i4.0
		add
		ret
	}

	// A method that locallocs and then returns a value: the return form has to
	// release the block.
	.method public static int32 test_3_ret_after_localloc () cil managed
	{
		.maxstack 3
		.locals init (native int block)
		ldc.i4.s 32
		localloc
		stloc.0
		ldloc.0
		ldc.i4.3
		stind.i4
		ldloc.0
		ldind.i4
		ret
	}

	.method public static valuetype LocalsPair MakesPairAfterLocalloc () cil managed noinlining
	{
		.maxstack 3
		.locals init (native int block, valuetype LocalsPair p)
		ldc.i4.s 16
		localloc
		stloc.0
		ldloca.s 1
		ldc.i4.6
		stfld int32 LocalsPair::first
		ldloca.s 1
		ldc.i4.1
		conv.i8
		stfld int64 LocalsPair::second
		ldloc.1
		ret
	}

	.method public static int32 test_7_ret_valuetype_after_localloc () cil managed
	{
		.maxstack 3
		.locals init (valuetype LocalsPair p)
		call valuetype LocalsPair LocalsIL::MakesPairAfterLocalloc ()
		stloc.0
		ldloc.0
		ldfld int32 LocalsPair::first
		ldloc.0
		ldfld int64 LocalsPair::second
		conv.i4
		add
		ret
	}

	.method public static void VoidAfterLocalloc () cil managed noinlining
	{
		.maxstack 3
		.locals init (native int block)
		ldc.i4.s 16
		localloc
		stloc.0
		ldloc.0
		ldc.i4.1
		stind.i4
		ret
	}

	.method public static int32 test_2_ret_void_after_localloc () cil managed
	{
		.maxstack 2
		call void LocalsIL::VoidAfterLocalloc ()
		ldc.i4.2
		ret
	}

	// A localloc inside a loop: each turn reuses the same block rather than
	// growing the frame.
	.method public static int32 test_16_localloc_in_a_loop () cil managed
	{
		.maxstack 4
		.locals init (int32 i, int32 total, native int block)
		ldc.i4.0
		stloc.0
		ldc.i4.0
		stloc.1
	IL_loop:
		ldloc.0
		ldc.i4.4
		bge IL_done
		ldc.i4.s 64
		localloc
		stloc.2
		ldloc.2
		ldloc.0
		stind.i4
		ldloc.1
		ldloc.2
		ldind.i4
		ldc.i4.1
		add
		add
		stloc.1
		ldloc.0
		ldc.i4.1
		add
		stloc.0
		br IL_loop
	IL_done:
		ldloc.1
		ldc.i4.s 6
		add
		ret
	}
}
