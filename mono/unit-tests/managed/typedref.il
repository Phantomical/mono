// Typed references and the memory prefixes.  C# reaches none of these.

.assembly extern mscorlib { }

.class public sequential ansi TypedRefPair extends [mscorlib]System.ValueType
{
	.field public int32 first
	.field public int32 second
}

.class public auto ansi TypedRefs extends [mscorlib]System.Object
{
	.method public static int32 test_42_mkrefany_refanyval () cil managed
	{
		.maxstack 2
		.locals init (int32 held)
		ldc.i4 42
		stloc.0
		ldloca.s 0
		mkrefany [mscorlib]System.Int32
		refanyval [mscorlib]System.Int32
		ldind.i4
		ret
	}

	.method public static int32 test_1_refanytype () cil managed
	{
		.maxstack 2
		.locals init (int32 held)
		ldloca.s 0
		mkrefany [mscorlib]System.Int32
		refanytype
		call class [mscorlib]System.Type [mscorlib]System.Type::GetTypeFromHandle (valuetype [mscorlib]System.RuntimeTypeHandle)
		ldtoken [mscorlib]System.Int32
		call class [mscorlib]System.Type [mscorlib]System.Type::GetTypeFromHandle (valuetype [mscorlib]System.RuntimeTypeHandle)
		ceq
		ret
	}

	// refanyval against a type the reference was not made with.
	.method public static int32 test_1_refanyval_wrong_type_throws () cil managed
	{
		.maxstack 2
		.locals init (int32 held)
		.try {
			ldloca.s 0
			mkrefany [mscorlib]System.Int32
			refanyval [mscorlib]System.Int64
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.InvalidCastException {
			pop
			ldc.i4.1
			ret
		}
	}

	.method public static int32 test_3_unaligned_load () cil managed
	{
		.maxstack 3
		.locals init (int64 buffer, int32 read)
		ldloca.s 0
		ldc.i4.3
		conv.i8
		stind.i8
		ldloca.s 0
		unaligned. 1
		ldind.i4
		ret
	}

	.method public static int32 test_4_volatile_store () cil managed
	{
		.maxstack 3
		.locals init (int32 held)
		ldloca.s 0
		ldc.i4.4
		volatile.
		stind.i4
		ldloca.s 0
		volatile.
		ldind.i4
		ret
	}

	// cpblk and initblk over two stack buffers.
	.method public static int32 test_255_cpblk () cil managed
	{
		.maxstack 4
		.locals init (int64 source, int64 destination)
		ldloca.s 0
		ldc.i4 255
		ldc.i4.8
		initblk
		ldloca.s 1
		ldloca.s 0
		ldc.i4.8
		cpblk
		ldloca.s 1
		ldind.u1
		ret
	}

	.method public static int32 test_0_initblk_zeroes () cil managed
	{
		.maxstack 4
		.locals init (int64 buffer)
		ldloca.s 0
		ldc.i8 -1
		stind.i8
		ldloca.s 0
		ldc.i4.0
		ldc.i4.8
		initblk
		ldloca.s 0
		ldind.i8
		conv.i4
		ret
	}

	.method public static int32 test_7_localloc () cil managed
	{
		.maxstack 3
		.locals init (native int block)
		ldc.i4 32
		localloc
		stloc.0
		ldloc.0
		ldc.i4.7
		stind.i4
		ldloc.0
		ldind.i4
		ret
	}
}
