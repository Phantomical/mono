// cpblk and initblk: untyped block copies and fills, with and without prefixes.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'blocks' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'blocks.dll'

.class public abstract auto ansi sealed beforefieldinit Blocks extends [mscorlib]System.Object
{
	.method public static void Copy (native int d, native int s, int32 n) cil managed
	{
		.maxstack 3
		ldarg.0
		ldarg.1
		ldarg.2
		cpblk
		ret
	}

	.method public static void CopyUnaligned (native int d, native int s, int32 n) cil managed
	{
		.maxstack 3
		ldarg.0
		ldarg.1
		ldarg.2
		unaligned. 1
		cpblk
		ret
	}

	.method public static void Fill (native int p, int32 v, int32 n) cil managed
	{
		.maxstack 3
		ldarg.0
		ldarg.1
		ldarg.2
		initblk
		ret
	}

	.method public static void FillVolatile (native int p, int32 v, int32 n) cil managed
	{
		.maxstack 3
		ldarg.0
		ldarg.1
		ldarg.2
		volatile.
		initblk
		ret
	}
}
