// The IL prefixes: volatile. and unaligned. on memory accesses, constrained. on
// callvirt, tail. consumed as the hint it is.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'prefixed' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'prefixed.dll'

.class public auto ansi beforefieldinit Chatty extends [mscorlib]System.Object
{
	.field public static int32 Shared

	.method public hidebysig specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ret
	}

	.method public hidebysig newslot virtual instance int32 Talk () cil managed
	{
		.maxstack 1
		ldc.i4.2
		ret
	}
}

.class public sequential ansi sealed beforefieldinit Wrapped extends [mscorlib]System.ValueType
{
	.field public int32 Inner

	.method public hidebysig virtual instance string ToString () cil managed
	{
		.maxstack 1
		ldstr "wrapped"
		ret
	}
}

.class public abstract auto ansi sealed beforefieldinit Prefixed extends [mscorlib]System.Object
{
	.method public static int32 VolatileRead (native int p) cil managed
	{
		.maxstack 1
		ldarg.0
		volatile.
		ldind.i4
		ret
	}

	.method public static void VolatileWrite (native int p, int32 v) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		volatile.
		stind.i4
		ret
	}

	.method public static int32 VolatileStatic () cil managed
	{
		.maxstack 1
		volatile.
		ldsfld int32 Chatty::Shared
		ret
	}

	.method public static int64 UnalignedRead (native int p) cil managed
	{
		.maxstack 1
		ldarg.0
		unaligned. 1
		ldind.i8
		ret
	}

	.method public static int32 ConstrainedOnClass (class Chatty c) cil managed
	{
		.maxstack 1
		.locals init (class Chatty l)
		ldarg.0
		stloc.0
		ldloca.s 0
		constrained. Chatty
		callvirt instance int32 Chatty::Talk ()
		ret
	}

	.method public static string ConstrainedOnStruct (valuetype Wrapped w) cil managed
	{
		.maxstack 1
		ldarga.s 0
		constrained. Wrapped
		callvirt instance string [mscorlib]System.Object::ToString ()
		ret
	}

	.method public static int32 TailCall (int32 x) cil managed
	{
		.maxstack 1
		ldarg.0
		tail.
		call int32 Prefixed::Plain (int32)
		ret
	}

	.method public static int32 Plain (int32 x) cil managed
	{
		.maxstack 1
		ldarg.0
		ret
	}
}
