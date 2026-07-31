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
}
