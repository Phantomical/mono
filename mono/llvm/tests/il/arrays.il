// Array access: length, element load/store, element address and allocation.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'arrays' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'arrays.dll'

.class public abstract auto ansi sealed beforefieldinit Arrays extends [mscorlib]System.Object
{
	.method public static int32 Deref (int32& p) cil managed
	{
		.maxstack 1
		ldc.i4.0
		ret
	}

	.method public static int32 Length (int32[] a) cil managed
	{
		.maxstack 1
		ldarg.0
		ldlen
		conv.i4
		ret
	}

	.method public static int32 GetInt (int32[] a, int32 i) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		ldelem.i4
		ret
	}

	.method public static int32 GetByte (unsigned int8[] a, int32 i) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		ldelem.u1
		ret
	}

	.method public static float64 GetDouble (float64[] a, int32 i) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		ldelem.r8
		ret
	}

	.method public static object GetRef (object[] a, int32 i) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		ldelem.ref
		ret
	}

	.method public static void SetInt (int32[] a, int32 i, int32 v) cil managed
	{
		.maxstack 3
		ldarg.0
		ldarg.1
		ldarg.2
		stelem.i4
		ret
	}

	.method public static void SetRef (object[] a, int32 i, object v) cil managed
	{
		.maxstack 3
		ldarg.0
		ldarg.1
		ldarg.2
		stelem.ref
		ret
	}

	.method public static int32[] Make (int32 n) cil managed
	{
		.maxstack 1
		ldarg.0
		newarr [mscorlib]System.Int32
		ret
	}

	.method public static int32 ElementAddress (int32[] a, int32 i) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		ldelema [mscorlib]System.Int32
		call int32 Arrays::Deref (int32&)
		ret
	}

	.method public static int32 Sum (int32[] a) cil managed
	{
		.maxstack 3
		.locals init (int32 total, int32 i)
		ldc.i4.0
		stloc.0
		ldc.i4.0
		stloc.1
		br.s TEST
	LOOP:
		ldloc.0
		ldarg.0
		ldloc.1
		ldelem.i4
		add
		stloc.0
		ldloc.1
		ldc.i4.1
		add
		stloc.1
	TEST:
		ldloc.1
		ldarg.0
		ldlen
		conv.i4
		blt.s LOOP
		ldloc.0
		ret
	}
}
