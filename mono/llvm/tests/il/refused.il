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

	.method public static string ConstrainedBoxes (valuetype Plain p) cil managed
	{
		.maxstack 1
		ldarga.s 0
		constrained. Plain
		callvirt instance string [mscorlib]System.Object::ToString ()
		ret
	}

	.method public static void UsesJmp () cil managed
	{
		.maxstack 0
		jmp void Refused::Target ()
	}

	.method public static void Target () cil managed
	{
		.maxstack 0
		ret
	}

	.method public static vararg int32 UsesArglist () cil managed
	{
		.maxstack 1
		arglist
		pop
		ldc.i4.0
		ret
	}

	.method public static typedref UsesMkrefany (int32& p) cil managed
	{
		.maxstack 1
		ldarg.0
		mkrefany [mscorlib]System.Int32
		ret
	}
}

.class public sequential ansi sealed beforefieldinit Plain extends [mscorlib]System.ValueType
{
	.field public int32 Value
}
