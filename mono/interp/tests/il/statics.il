// A static initonly field read through a method built before the initializer
// finished.
//
// The transform folds an initonly scalar into the bytecode of whatever method
// reads it. That is sound once the value is final, since ECMA-335 II.16.1 lets
// nothing but the initializer write one. It is not sound before: Read () below
// is built while the .cctor runs, and the one body it gets is what every later
// call to it runs.

.assembly extern mscorlib { }

.class public auto ansi Statics extends [mscorlib]System.Object
{
	.field public static initonly int32 frozen
	.field public static int32 ordinary
	// What Read () answered from inside the initializer.
	.field public static int32 during

	.method private static void .cctor () cil managed
	{
		.maxstack 2
		// Builds Read () against a value the initializer has not written yet.
		call int32 Statics::Read ()
		stsfld int32 Statics::during
		ldc.i4.s 24
		stsfld int32 Statics::frozen
		ldc.i4.s 25
		stsfld int32 Statics::ordinary
		ret
	}

	// noinlining, so both callers run the one body rather than a copy the
	// transform can fold separately.
	.method public static int32 Read () cil managed noinlining
	{
		.maxstack 1
		ldsfld int32 Statics::frozen
		ret
	}

	.method public static int32 test_0_initonly_is_unwritten_inside_the_cctor () cil managed
	{
		.maxstack 1
		ldsfld int32 Statics::during
		ret
	}

	.method public static int32 test_24_initonly_read_after_the_cctor () cil managed
	{
		.maxstack 1
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
