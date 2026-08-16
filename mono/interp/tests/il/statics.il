// A static initonly field written through its own address.
//
// initonly restricts what a store may name (ECMA-335 II.16.1); it does not let
// a reader cache the value. The transform folds an initonly scalar into the
// bytecode of whatever method reads it, so a write that lands after that method
// was built is the case that separates the two engines.

.assembly extern mscorlib { }
.assembly 'statics' { }

.class public auto ansi Statics extends [mscorlib]System.Object
{
	.field public static initonly int32 frozen
	.field public static int32 ordinary

	.method private static void .cctor () cil managed
	{
		.maxstack 2
		ldc.i4.s 24
		stsfld int32 Statics::frozen
		ldc.i4.s 25
		stsfld int32 Statics::ordinary
		ret
	}

	.method public static int32 Read () cil managed
	{
		.maxstack 1
		ldsfld int32 Statics::frozen
		ret
	}

	.method public static int32 test_99_initonly_write_is_seen () cil managed
	{
		.maxstack 2
		// Build Read () against the value the initializer left.
		call int32 Statics::Read ()
		pop
		ldsflda int32 Statics::frozen
		ldc.i4.s 99
		stind.i4
		call int32 Statics::Read ()
		ret
	}

	.method public static int32 test_25_ordinary_static_reads () cil managed
	{
		.maxstack 1
		ldsfld int32 Statics::ordinary
		ret
	}

	.method public static int32 test_1_static_address_is_stable () cil managed
	{
		.maxstack 2
		ldsflda int32 Statics::ordinary
		ldsflda int32 Statics::ordinary
		ceq
		ret
	}
}
