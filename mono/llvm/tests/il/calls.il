// Calls: static, instance, and the two callvirt shapes - one that has to read the
// vtable and one the runtime already resolved to a single target.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'calls' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'calls.dll'

.class public auto ansi beforefieldinit Base extends [mscorlib]System.Object
{
	.method public hidebysig specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ret
	}

	.method public newslot virtual instance int32 Virt (int32 x) cil managed
	{
		.maxstack 1
		ldarg.1
		ret
	}

	.method public newslot virtual final instance int32 Sealed (int32 x) cil managed
	{
		.maxstack 1
		ldarg.1
		ret
	}

	.method public instance int32 Inst (int32 x) cil managed
	{
		.maxstack 2
		ldarg.1
		ldc.i4.1
		add
		ret
	}

	.method public newslot virtual instance !!0 Choose<T> (!!0 a, !!0 b) cil managed
	{
		.maxstack 1
		ldarg.1
		ret
	}
}

.class interface public abstract auto ansi IPicker
{
	.method public hidebysig newslot abstract virtual instance !!0 Pick<T> (!!0 a, !!0 b) cil managed {}
}

.class public abstract auto ansi sealed beforefieldinit Calls extends [mscorlib]System.Object
{
	.method public static int32 Helper (int32 x) cil managed
	{
		.maxstack 2
		ldarg.0
		ldc.i4.2
		mul
		ret
	}

	.method public static void Sink (int32 x) cil managed
	{
		.maxstack 0
		ret
	}

	.method public static int32 CallStatic (int32 x) cil managed
	{
		.maxstack 1
		ldarg.0
		call int32 Calls::Helper (int32)
		ret
	}

	.method public static int32 CallStaticTwice (int32 x) cil managed
	{
		.maxstack 2
		ldarg.0
		call int32 Calls::Helper (int32)
		call int32 Calls::Helper (int32)
		ret
	}

	.method public static void CallVoid (int32 x) cil managed
	{
		.maxstack 1
		ldarg.0
		call void Calls::Sink (int32)
		ret
	}

	.method public static int32 CallInstance (class Base b, int32 x) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		call instance int32 Base::Inst (int32)
		ret
	}

	.method public static int32 CallVirtual (class Base b, int32 x) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		callvirt instance int32 Base::Virt (int32)
		ret
	}

	.method public static int32 CallSealed (class Base b, int32 x) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		callvirt instance int32 Base::Sealed (int32)
		ret
	}

	.method public static int32 CallNonVirtual (class Base b, int32 x) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		callvirt instance int32 Base::Inst (int32)
		ret
	}

	.method public static void CallInterface (class [mscorlib]System.IDisposable d) cil managed
	{
		.maxstack 1
		ldarg.0
		callvirt instance void [mscorlib]System.IDisposable::Dispose ()
		ret
	}

	.method public static int32 CallGenericVirtual (class Base b) cil managed
	{
		.maxstack 3
		ldarg.0
		ldc.i4.1
		ldc.i4.2
		callvirt instance !!0 Base::Choose<int32> (!!0, !!0)
		ret
	}

	.method public static int32 CallGenericInterface (class IPicker p) cil managed
	{
		.maxstack 3
		ldarg.0
		ldc.i4.1
		ldc.i4.2
		callvirt instance !!0 IPicker::Pick<int32> (!!0, !!0)
		ret
	}

	.method public static int32 Deref (int32& p) cil managed
	{
		.maxstack 1
		ldc.i4.0
		ret
	}

	.method public static int32 TailStatic (int32 x) cil managed
	{
		.maxstack 1
		ldarg.0
		tail.
		call int32 Calls::Helper (int32)
		ret
	}

	.method public static void TailVoid (int32 x) cil managed
	{
		.maxstack 1
		ldarg.0
		tail.
		call void Calls::Sink (int32)
		ret
	}

	.method public static int32 TailVirtual (class Base b, int32 x) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		tail.
		callvirt instance int32 Base::Virt (int32)
		ret
	}

	// The prototypes differ, so the prefix is declined and the call stays put.
	.method public static int32 TailMismatch (int32 a, int32 b) cil managed
	{
		.maxstack 1
		ldarg.0
		tail.
		call int32 Calls::Helper (int32)
		ret
	}

	// A managed pointer among the arguments could point into this frame, so the
	// prefix is declined.
	.method public static int32 TailByref (int32& p) cil managed
	{
		.maxstack 1
		ldarg.0
		tail.
		call int32 Calls::Deref (int32&)
		ret
	}
}
