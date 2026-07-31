// ldftn, ldvirtftn and calli: method pointers made, looked up, and called through.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'fnptr' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'fnptr.dll'

.class public auto ansi beforefieldinit Speaker extends [mscorlib]System.Object
{
	.method public hidebysig specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ret
	}

	.method public hidebysig newslot virtual instance int32 Speak () cil managed
	{
		.maxstack 1
		ldc.i4.1
		ret
	}
}

.class public abstract auto ansi sealed beforefieldinit Fnptr extends [mscorlib]System.Object
{
	.method public static int32 AddOne (int32 x) cil managed
	{
		.maxstack 2
		ldarg.0
		ldc.i4.1
		add
		ret
	}

	.method public static native int TakeStatic () cil managed
	{
		.maxstack 1
		ldftn int32 Fnptr::AddOne (int32)
		ret
	}

	.method public static native int TakeVirtual (class Speaker s) cil managed
	{
		.maxstack 1
		ldarg.0
		ldvirtftn instance int32 Speaker::Speak ()
		ret
	}

	.method public static int32 CallThroughPointer (int32 x) cil managed
	{
		.maxstack 2
		ldarg.0
		ldftn int32 Fnptr::AddOne (int32)
		calli int32 (int32)
		ret
	}

	.method public static int32 CallThroughArgument (native int f, int32 x) cil managed
	{
		.maxstack 2
		ldarg.1
		ldarg.0
		calli int32 (int32)
		ret
	}

	.method public static int32 CallNative (native int f, int32 x) cil managed
	{
		.maxstack 2
		ldarg.1
		ldarg.0
		calli unmanaged cdecl int32 (int32)
		ret
	}

	.method public static int32 TailThroughPointer (int32 x) cil managed
	{
		.maxstack 2
		ldarg.0
		ldftn int32 Fnptr::AddOne (int32)
		tail.
		calli int32 (int32)
		ret
	}
}
