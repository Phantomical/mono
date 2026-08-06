// Control flow: the block-leader pre-pass, the back edge of a loop, a join
// entered with a non-empty evaluation stack, and switch.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'flow' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'flow.dll'

.class public abstract auto ansi sealed beforefieldinit Flow extends [mscorlib]System.Object
{
	.method public static int32 Max (int32 a, int32 b) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		bge.s FIRST
		ldarg.1
		ret
	FIRST:
		ldarg.0
		ret
	}

	.method public static int32 SumTo (int32 n) cil managed
	{
		.maxstack 2
		.locals init (int32 sum, int32 i)
		ldc.i4.0
		stloc.0
		ldc.i4.0
		stloc.1
		br.s TEST
	LOOP:
		ldloc.0
		ldloc.1
		add
		stloc.0
		ldloc.1
		ldc.i4.1
		add
		stloc.1
	TEST:
		ldloc.1
		ldarg.0
		blt.s LOOP
		ldloc.0
		ret
	}

	// Both edges into JOIN arrive holding a value the block goes on to use, which
	// is what makes the spill slots do any work.
	.method public static int32 StackAcrossBranch (int32 x) cil managed
	{
		.maxstack 3
		ldc.i4.1
		ldarg.0
		brtrue.s TAKEN
		ldc.i4.2
		br.s JOIN
	TAKEN:
		ldc.i4.3
	JOIN:
		add
		ret
	}

	.method public static int32 Choose (int32 k) cil managed
	{
		.maxstack 1
		ldarg.0
		switch (CASE0, CASE1, CASE2)
		ldc.i4.m1
		ret
	CASE0:
		ldc.i4.s 10
		ret
	CASE1:
		ldc.i4.s 20
		ret
	CASE2:
		ldc.i4.s 30
		ret
	}

	.method public static int32 Compare (int32 a, int32 b) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		beq.s EQ
		ldarg.0
		ldarg.1
		bgt.s GT
		ldarg.0
		ldarg.1
		blt.un.s LTU
		ldarg.0
		ldarg.1
		bne.un.s NE
		ldc.i4.0
		ret
	EQ:
		ldc.i4.1
		ret
	GT:
		ldc.i4.2
		ret
	LTU:
		ldc.i4.3
		ret
	NE:
		ldc.i4.4
		ret
	}

	// The comparison instructions that leave a 0/1 behind rather than branching.
	.method public static int32 Compares (int32 a, int32 b) cil managed
	{
		.maxstack 3
		ldarg.0
		ldarg.1
		ceq
		ldarg.0
		ldarg.1
		cgt
		add
		ldarg.0
		ldarg.1
		clt
		add
		ldarg.0
		ldarg.1
		cgt.un
		add
		ldarg.0
		ldarg.1
		clt.un
		add
		ret
	}

	.method public static int32 Nested (int32 a, int32 b) cil managed
	{
		.maxstack 2
		.locals init (int32 r)
		ldc.i4.0
		stloc.0
		ldarg.0
		brfalse.s OUTER
		ldarg.1
		brfalse.s OUTER
		ldloc.0
		ldc.i4.1
		add
		stloc.0
	OUTER:
		ldloc.0
		ldarg.0
		ldarg.1
		ble.un.s SKIP
		ldc.i4.s 100
		add
		ret
	SKIP:
		ret
	}

	// The two paths into JOIN leave different representations at depth 0: an
	// int32 down one and a native int down the other. The slot keeps the type
	// the first path gave it and the other edge converts on the way in.
	.method public static native int MergeIntWithNativeInt (int32 c, native int p) cil managed
	{
		.maxstack 1
		ldarg.0
		brtrue.s WIDE
		ldc.i4.5
		br.s JOIN
	WIDE:
		ldarg.1
	JOIN:
		conv.i
		ret
	}

	.method public static float64 MergeFloatWidths (int32 c, float32 f, float64 d) cil managed
	{
		.maxstack 1
		ldarg.0
		brtrue.s NARROW
		ldarg.2
		br.s JOIN
	NARROW:
		ldarg.1
	JOIN:
		conv.r8
		ret
	}

	.method public static native int MergePointerWithNativeInt (int32 c, int32& r, native int p) cil managed
	{
		.maxstack 1
		ldarg.0
		brtrue.s PTR
		ldarg.2
		br.s JOIN
	PTR:
		ldarg.1
	JOIN:
		conv.i
		ret
	}

	// A block nothing reaches has no entry stack, so there is nothing to translate
	// its body against. The dead pop below would underflow a fabricated empty one,
	// and its fallthrough would tell JOIN it is entered holding nothing - which the
	// live edge from brtrue, which arrives holding a value, then disagrees with.
	.method public static int32 DeadBlockBeforeJoin (int32 x) cil managed
	{
		.maxstack 2
		ldc.i4.1
		ldarg.0
		brtrue.s TAKEN
		ldc.i4.2
		br.s JOIN
		pop
	TAKEN:
		ldc.i4.3
	JOIN:
		add
		ret
	}
}
