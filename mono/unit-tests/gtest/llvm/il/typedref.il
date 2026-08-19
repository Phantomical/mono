// Typed references: built with mkrefany, unpacked again with refanyval and
// refanytype.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'typedref' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'typedref.dll'

.class public abstract auto ansi sealed beforefieldinit TypedRef extends [mscorlib]System.Object
{
	.method public static typedref MakeRef (int32& p) cil managed
	{
		.maxstack 1
		ldarg.0
		mkrefany [mscorlib]System.Int32
		ret
	}

	.method public static int32 ReadBack (int32& p) cil managed
	{
		.maxstack 1
		ldarg.0
		mkrefany [mscorlib]System.Int32
		refanyval [mscorlib]System.Int32
		ldind.i4
		ret
	}

	.method public static valuetype [mscorlib]System.RuntimeTypeHandle TypeOfRef (typedref tr) cil managed
	{
		.maxstack 1
		ldarg.0
		refanytype
		ret
	}

	.method public static int32 ValueOfRef (typedref tr) cil managed
	{
		.maxstack 1
		ldarg.0
		refanyval [mscorlib]System.Int32
		ldind.i4
		ret
	}

	// A vararg method reaches its variable arguments through arglist, which
	// hands back the buffer the caller filled.
	.method public static vararg int32 CountVarargs (int32 first) cil managed
	{
		.maxstack 2
		.locals init (valuetype [mscorlib]System.ArgIterator it)

		ldloca.s 0
		arglist
		call instance void [mscorlib]System.ArgIterator::.ctor (valuetype [mscorlib]System.RuntimeArgumentHandle)
		ldloca.s 0
		call instance int32 [mscorlib]System.ArgIterator::GetRemainingCount ()
		ret
	}

	// The variable part leaves the argument list for that buffer: a float takes
	// four bytes of it, so the double behind it starts at 8 + 8 + 4.
	.method public static int32 CallsAVararg () cil managed
	{
		.maxstack 4
		ldc.i4.1
		ldc.i4.2
		ldc.r4 3.5
		ldc.r8 6.75
		call vararg int32 TypedRef::CountVarargs (int32, ..., int32, float32, float64)
		ret
	}
}
