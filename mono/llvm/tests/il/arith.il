// Arithmetic, conversions and bit operations, one shape per method so a failure
// names the instruction group that broke.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'arith' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'arith.dll'

.class public abstract auto ansi sealed beforefieldinit Arith extends [mscorlib]System.Object
{
	.method public static int32 Add (int32 a, int32 b) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		add
		ret
	}

	.method public static int64 MixInt64 (int64 a, int64 b) cil managed
	{
		.maxstack 3
		ldarg.0
		ldarg.1
		mul
		ldarg.0
		sub
		ret
	}

	.method public static float64 MixFloat (float64 a, float64 b) cil managed
	{
		.maxstack 3
		ldarg.0
		ldarg.1
		div
		ldarg.1
		add
		ret
	}

	// div and rem carry the divide-by-zero and overflow guards, so these two are
	// where the conditional-throw machinery first shows up.
	.method public static int32 Div (int32 a, int32 b) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		div
		ret
	}

	.method public static int32 RemUn (int32 a, int32 b) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		rem.un
		ret
	}

	.method public static int32 AddOvf (int32 a, int32 b) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		add.ovf
		ret
	}

	.method public static int32 MulOvfUn (int32 a, int32 b) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		mul.ovf.un
		ret
	}

	.method public static int32 SubOvf (int32 a, int32 b) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		sub.ovf
		ret
	}

	.method public static int32 Neg (int32 a) cil managed
	{
		.maxstack 1
		ldarg.0
		neg
		ret
	}

	.method public static float64 NegFloat (float64 a) cil managed
	{
		.maxstack 1
		ldarg.0
		neg
		ret
	}

	.method public static int32 Bits (int32 a, int32 b) cil managed
	{
		.maxstack 3
		ldarg.0
		ldarg.1
		and
		ldarg.0
		or
		ldarg.1
		xor
		not
		ret
	}

	.method public static int32 Shifts (int32 a, int32 b) cil managed
	{
		.maxstack 3
		ldarg.0
		ldarg.1
		shl
		ldarg.1
		shr
		ldarg.1
		shr.un
		ret
	}

	.method public static float64 ConvToFloat (int64 a) cil managed
	{
		.maxstack 1
		ldarg.0
		conv.r8
		ret
	}

	.method public static int32 ConvNarrow (int64 a) cil managed
	{
		.maxstack 1
		ldarg.0
		conv.i2
		ret
	}

	.method public static int32 ConvOvf (int64 a) cil managed
	{
		.maxstack 1
		ldarg.0
		conv.ovf.i4
		ret
	}

	.method public static float64 ConvRUn (int64 a) cil managed
	{
		.maxstack 1
		ldarg.0
		conv.r.un
		ret
	}

	.method public static int64 ConvOvfLongFromDouble (float64 a) cil managed
	{
		.maxstack 1
		ldarg.0
		conv.ovf.i8
		ret
	}

	.method public static uint64 ConvOvfULongFromDouble (float64 a) cil managed
	{
		.maxstack 1
		ldarg.0
		conv.ovf.u8
		ret
	}

	.method public static int64 Constants () cil managed
	{
		.maxstack 2
		ldc.i4.s 42
		conv.i8
		ldc.i8 0x100000000
		add
		ret
	}

	.method public static float64 FloatConstants () cil managed
	{
		.maxstack 2
		ldc.r4 1.5
		conv.r8
		ldc.r8 2.25
		mul
		ret
	}
}
