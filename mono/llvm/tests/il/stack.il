// The instructions that only rearrange the evaluation stack.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'stack' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'stack.dll'

.class public abstract auto ansi sealed beforefieldinit Stack extends [mscorlib]System.Object
{
	.method public static int32 Dup (int32 x) cil managed
	{
		.maxstack 2
		ldarg.0
		dup
		add
		ret
	}

	.method public static int32 Pop (int32 a, int32 b) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		pop
		ret
	}

	.method public static object Null () cil managed
	{
		.maxstack 1
		ldnull
		ret
	}

	// A dup whose copies are used on both sides of a branch, so the value has to
	// survive the spill/reload the join goes through.
	.method public static int32 DupAcrossBranch (int32 x) cil managed
	{
		.maxstack 3
		ldarg.0
		dup
		brtrue.s TAKEN
		ldc.i4.1
		add
		ret
	TAKEN:
		ldc.i4.2
		mul
		ret
	}
}
