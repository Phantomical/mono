// newobj: allocate-and-construct for classes, construct-in-place for value types.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'objects' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'objects.dll'

.class public auto ansi beforefieldinit Counter extends [mscorlib]System.Object
{
	.field public int32 Count

	.method public hidebysig specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ret
	}

	.method public hidebysig specialname rtspecialname instance void .ctor (int32 start) cil managed
	{
		.maxstack 2
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ldarg.0
		ldarg.1
		stfld int32 Counter::Count
		ret
	}
}

.class public sequential ansi sealed beforefieldinit Point extends [mscorlib]System.ValueType
{
	.field public int32 X
	.field public int32 Y

	.method public hidebysig specialname rtspecialname instance void .ctor (int32 x, int32 y) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		stfld int32 Point::X
		ldarg.0
		ldarg.2
		stfld int32 Point::Y
		ret
	}
}

.class public abstract auto ansi sealed beforefieldinit Objects extends [mscorlib]System.Object
{
	.method public static class Counter MakeCounter () cil managed
	{
		.maxstack 1
		newobj instance void Counter::.ctor ()
		ret
	}

	.method public static class Counter MakeCounterAt (int32 start) cil managed
	{
		.maxstack 1
		ldarg.0
		newobj instance void Counter::.ctor (int32)
		ret
	}

	.method public static valuetype Point MakePoint (int32 x, int32 y) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		newobj instance void Point::.ctor (int32, int32)
		ret
	}

	.method public static int32 UseThePoint (int32 x) cil managed
	{
		.maxstack 2
		.locals init (valuetype Point p)
		ldarg.0
		ldc.i4.5
		newobj instance void Point::.ctor (int32, int32)
		stloc.0
		ldloca.s 0
		ldfld int32 Point::X
		ret
	}
}
