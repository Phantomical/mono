// sizeof, ckfinite, break and localloc.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'misc' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'misc.dll'

.class public sequential ansi sealed beforefieldinit Wide extends [mscorlib]System.ValueType
{
	.field public int64 A
	.field public int64 B
}

.class public abstract auto ansi sealed beforefieldinit Misc extends [mscorlib]System.Object
{
	.method public static int32 SizeOfInt () cil managed
	{
		.maxstack 1
		sizeof [mscorlib]System.Int32
		ret
	}

	.method public static int32 SizeOfObject () cil managed
	{
		.maxstack 1
		sizeof [mscorlib]System.Object
		ret
	}

	.method public static int32 SizeOfWide () cil managed
	{
		.maxstack 1
		sizeof Wide
		ret
	}

	.method public static float64 CheckFinite (float64 d) cil managed
	{
		.maxstack 1
		ldarg.0
		ckfinite
		ret
	}

	.method public static int32 Breakpoint (int32 x) cil managed
	{
		.maxstack 1
		break
		ldarg.0
		ret
	}

	.method public static native int StackAlloc (int32 n) cil managed
	{
		.maxstack 1
		ldarg.0
		localloc
		ret
	}

	.method public static native int StackAllocZeroed (int32 n) cil managed
	{
		.maxstack 1
		.locals init (int32 unused)
		ldarg.0
		localloc
		ret
	}
}
