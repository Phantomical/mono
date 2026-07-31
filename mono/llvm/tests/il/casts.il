// castclass and isinst, over classes, interfaces and the boxed form of a value type.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'casts' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'casts.dll'

.class interface public abstract auto ansi IThing
{
	.method public hidebysig newslot abstract virtual instance int32 Value () cil managed {}
}

.class public auto ansi beforefieldinit Impl extends [mscorlib]System.Object implements IThing
{
	.method public hidebysig specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ret
	}

	.method public final hidebysig newslot virtual instance int32 Value () cil managed
	{
		.maxstack 1
		ldc.i4.7
		ret
	}
}

.class public abstract auto ansi sealed beforefieldinit Casts extends [mscorlib]System.Object
{
	.method public static string CastString (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		castclass [mscorlib]System.String
		ret
	}

	.method public static object IsString (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		isinst [mscorlib]System.String
		ret
	}

	.method public static class IThing CastIface (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		castclass IThing
		ret
	}

	.method public static object IsIface (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		isinst IThing
		ret
	}

	.method public static object IsBoxedInt (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		isinst [mscorlib]System.Int32
		ret
	}

	.method public static object TwoCasts (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		castclass [mscorlib]System.String
		castclass [mscorlib]System.Object
		ret
	}

	.method public static string UnboxAnyString (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		unbox.any [mscorlib]System.String
		ret
	}
}
