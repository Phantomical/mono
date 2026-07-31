// Methods the translator is expected to turn down.  Each one should come back as
// an llvm::Error naming what it could not do, rather than as a crash or as IR
// that only fails later.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'refused' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'refused.dll'

.class public abstract auto ansi sealed beforefieldinit Refused extends [mscorlib]System.Object
{
	.method public static string UsesAStringCtor () cil managed
	{
		.maxstack 2
		ldc.i4.s 97
		ldc.i4.3
		newobj instance void [mscorlib]System.String::.ctor (char, int32)
		ret
	}

	.method public static int32 StackUnderflow () cil managed
	{
		.maxstack 2
		add
		ret
	}

	.method public static int32 BadLocalIndex () cil managed
	{
		.maxstack 1
		.locals init (int32 a)
		ldloc.s 5
		ret
	}

	.method public static int32 FallsOffTheEnd (int32 x) cil managed
	{
		.maxstack 1
		ldarg.0
		nop
	}

	.method public static object BoxesANullable (valuetype [mscorlib]System.Nullable`1<int32> v) cil managed
	{
		.maxstack 1
		ldarg.0
		box valuetype [mscorlib]System.Nullable`1<int32>
		ret
	}
}
