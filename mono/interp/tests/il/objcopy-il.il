// cpobj, ldobj, stobj and unbox, spelled out rather than left to a C# compiler
// to choose.

.assembly extern mscorlib { }

.class public sequential ansi sealed ObjCopyOne extends [mscorlib]System.ValueType
{
	.field public int8 a
}

.class public sequential ansi sealed ObjCopyFour extends [mscorlib]System.ValueType
{
	.field public int32 a
}

.class public sequential ansi sealed ObjCopyWide extends [mscorlib]System.ValueType
{
	.field public int64 a
	.field public int64 b
	.field public int64 c
}

.class public sequential ansi sealed ObjCopyMixed extends [mscorlib]System.ValueType
{
	.field public int32 count
	.field public string name
}

.class public auto ansi ObjCopyIL extends [mscorlib]System.Object
{
	.method public static int32 test_7_cpobj_one_byte () cil managed
	{
		.maxstack 3
		.locals init (valuetype ObjCopyOne source, valuetype ObjCopyOne target)
		ldloca.s 0
		ldc.i4.7
		stfld int8 ObjCopyOne::a
		ldloca.s 1
		ldloca.s 0
		cpobj ObjCopyOne
		ldloca.s 1
		ldfld int8 ObjCopyOne::a
		ret
	}

	.method public static int32 test_9_cpobj_four_bytes () cil managed
	{
		.maxstack 3
		.locals init (valuetype ObjCopyFour source, valuetype ObjCopyFour target)
		ldloca.s 0
		ldc.i4.s 9
		stfld int32 ObjCopyFour::a
		ldloca.s 1
		ldloca.s 0
		cpobj ObjCopyFour
		ldloca.s 1
		ldfld int32 ObjCopyFour::a
		ret
	}

	.method public static int32 test_6_cpobj_wide () cil managed
	{
		.maxstack 3
		.locals init (valuetype ObjCopyWide source, valuetype ObjCopyWide target)
		ldloca.s 0
		ldc.i4.1
		conv.i8
		stfld int64 ObjCopyWide::a
		ldloca.s 0
		ldc.i4.2
		conv.i8
		stfld int64 ObjCopyWide::b
		ldloca.s 0
		ldc.i4.3
		conv.i8
		stfld int64 ObjCopyWide::c
		ldloca.s 1
		ldloca.s 0
		cpobj ObjCopyWide
		ldloca.s 1
		ldfld int64 ObjCopyWide::a
		ldloca.s 1
		ldfld int64 ObjCopyWide::b
		add
		ldloca.s 1
		ldfld int64 ObjCopyWide::c
		add
		conv.i4
		ret
	}

	// A struct holding a reference: the copy needs the write barrier.
	.method public static int32 test_5_cpobj_with_a_reference () cil managed
	{
		.maxstack 3
		.locals init (valuetype ObjCopyMixed source, valuetype ObjCopyMixed target)
		ldloca.s 0
		ldc.i4.0
		stfld int32 ObjCopyMixed::count
		ldloca.s 0
		ldstr "hello"
		stfld string ObjCopyMixed::name
		ldloca.s 1
		ldloca.s 0
		cpobj ObjCopyMixed
		call void [mscorlib]System.GC::Collect ()
		ldloca.s 1
		ldfld string ObjCopyMixed::name
		callvirt instance int32 [mscorlib]System.String::get_Length ()
		ret
	}

	.method public static int32 test_1_cpobj_of_a_primitive () cil managed
	{
		.maxstack 3
		.locals init (int32 source, int32 target)
		ldc.i4.1
		stloc.0
		ldloca.s 1
		ldloca.s 0
		cpobj [mscorlib]System.Int32
		ldloc.1
		ret
	}

	.method public static int32 test_4_ldobj_and_stobj () cil managed
	{
		.maxstack 3
		.locals init (valuetype ObjCopyFour source, valuetype ObjCopyFour target)
		ldloca.s 0
		ldc.i4.4
		stfld int32 ObjCopyFour::a
		ldloca.s 1
		ldloca.s 0
		ldobj ObjCopyFour
		stobj ObjCopyFour
		ldloca.s 1
		ldfld int32 ObjCopyFour::a
		ret
	}

	// unbox hands back the address inside the box rather than a copy, so a write
	// through it is visible in the boxed value.
	.method public static int32 test_8_unbox_gives_an_interior_address () cil managed
	{
		.maxstack 3
		.locals init (object boxed, valuetype ObjCopyFour v)
		ldloca.s 1
		ldc.i4.3
		stfld int32 ObjCopyFour::a
		ldloc.1
		box ObjCopyFour
		stloc.0
		ldloc.0
		unbox ObjCopyFour
		ldc.i4.8
		stfld int32 ObjCopyFour::a
		ldloc.0
		unbox.any ObjCopyFour
		stloc.1
		ldloca.s 1
		ldfld int32 ObjCopyFour::a
		ret
	}

	.method public static int32 test_1_unbox_of_the_wrong_type_throws () cil managed
	{
		.maxstack 3
		.locals init (object boxed)
		.try {
			ldc.i4.3
			box [mscorlib]System.Int32
			stloc.0
			ldloc.0
			unbox ObjCopyFour
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.InvalidCastException {
			pop
			ldc.i4.1
			ret
		}
	}

	.method public static int32 test_1_unbox_of_null_throws () cil managed
	{
		.maxstack 3
		.try {
			ldnull
			unbox ObjCopyFour
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.NullReferenceException {
			pop
			ldc.i4.1
			ret
		}
	}

	// initobj over a struct holding a reference clears the reference too.
	.method public static int32 test_1_initobj_clears_a_reference () cil managed
	{
		.maxstack 3
		.locals init (valuetype ObjCopyMixed v)
		ldloca.s 0
		ldstr "set"
		stfld string ObjCopyMixed::name
		ldloca.s 0
		initobj ObjCopyMixed
		ldloca.s 0
		ldfld string ObjCopyMixed::name
		ldnull
		ceq
		ret
	}
}
