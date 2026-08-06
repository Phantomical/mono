// Locals and arguments: the round trip through their allocas, and the two
// instructions that hand out an address rather than a value.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'locals' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'locals.dll'

.class public abstract auto ansi sealed beforefieldinit Locals extends [mscorlib]System.Object
{
	// Something to hand a managed pointer to.  Its own body is never translated;
	// only its signature matters.
	.method public static int32 Deref (int32& p) cil managed
	{
		.maxstack 1
		ldc.i4.0
		ret
	}

	.method public static int32 RoundTrip (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int32 a, int64 b, float64 c)
		ldarg.0
		stloc.0
		ldloc.0
		conv.i8
		stloc.1
		ldloc.1
		conv.r8
		stloc.2
		ldloc.2
		conv.i4
		ret
	}

	// Locals narrower than a stack slot: the store truncates and the load widens.
	.method public static int32 NarrowLocals (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int8 a, unsigned int8 b, int16 c, unsigned int16 d)
		ldarg.0
		stloc.0
		ldarg.0
		stloc.1
		ldarg.0
		stloc.2
		ldarg.0
		stloc.3
		ldloc.0
		ldloc.1
		add
		ldloc.2
		add
		ldloc.3
		add
		ret
	}

	.method public static int32 StoreArg (int32 x) cil managed
	{
		.maxstack 2
		ldarg.0
		ldc.i4.1
		add
		starg.s x
		ldarg.0
		ret
	}

	.method public static int32 ArgAddress (int32 x) cil managed
	{
		.maxstack 1
		ldarga.s x
		call int32 Locals::Deref (int32&)
		ret
	}

	.method public static int32 LocalAddress () cil managed
	{
		.maxstack 1
		.locals init (int32 a)
		ldloca.s a
		call int32 Locals::Deref (int32&)
		ret
	}

	.method public static float64 ManyLocals (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int32 v0, int32 v1, int32 v2, int32 v3, int32 v4, float64 v5)
		ldarg.0
		stloc.s v4
		ldloc.s v4
		conv.r8
		stloc.s v5
		ldloc.s v5
		ret
	}
}
