/*
 * A value type too wide for the return registers comes back through a pointer the
 * caller passes. tailcall-return-valuetype.il checks that one such call really does
 * reuse its caller's frame, by comparing the addresses of two locals that have to
 * land in the same place.
 *
 * This one checks the property that buys: a million alternations between two methods
 * returning that value type run in the stack of one. Without the jump each hop keeps
 * its frame and the recursion runs the stack out long before it reaches the bottom,
 * so this fails by dying rather than by printing.
 *
 * Mutual recursion, because a method that tail calls itself can be rewritten into a
 * loop, which would run in bounded stack without a jump ever being made.
 */

.assembly extern mscorlib { }

.assembly 'tailcall-return-valuetype-deep' { }

.class public ValueType
extends [mscorlib]System.ValueType
{
.field public int64 a0
.field public int64 a1
.field public int64 a2
.field public int64 a3
.field public int64 a4
.field public int64 a5
.field public int64 a6
.field public int64 a7
.field public int64 a8
.field public int64 a9

// The bottom of the recursion: the value everything above hands back unchanged.
.method static valuetype ValueType Bottom () noinlining
{
	.maxstack 8
	.locals init ( valuetype ValueType V_0 )

	ldloca 0
	ldc.i8 0
	stfld int64 ValueType::a0
	ldloca 0
	ldc.i8 1
	stfld int64 ValueType::a1
	ldloca 0
	ldc.i8 2
	stfld int64 ValueType::a2
	ldloca 0
	ldc.i8 3
	stfld int64 ValueType::a3
	ldloca 0
	ldc.i8 4
	stfld int64 ValueType::a4
	ldloca 0
	ldc.i8 5
	stfld int64 ValueType::a5
	ldloca 0
	ldc.i8 6
	stfld int64 ValueType::a6
	ldloca 0
	ldc.i8 7
	stfld int64 ValueType::a7
	ldloca 0
	ldc.i8 8
	stfld int64 ValueType::a8
	ldloca 0
	ldc.i8 9
	stfld int64 ValueType::a9

	ldloca 0
	ldobj ValueType
	ret
}

.method public static valuetype ValueType Method1 (int64 depth) noinlining
{
	.maxstack 8

	ldarg 0
	ldc.i8 0
	ble Bottom

	ldarg 0
	ldc.i8 1
	sub
tail.
	call valuetype ValueType valuetype ValueType::Method2 (int64)
	ret

Bottom:
tail.
	call valuetype ValueType valuetype ValueType::Bottom ()
	ret
}

.method static valuetype ValueType Method2 (int64 depth) noinlining
{
	.maxstack 8

	ldarg 0
	ldc.i8 0
	ble Bottom

	ldarg 0
	ldc.i8 1
	sub
tail.
	call valuetype ValueType valuetype ValueType::Method1 (int64)
	ret

Bottom:
tail.
	call valuetype ValueType valuetype ValueType::Bottom ()
	ret
}
}

.class B
{

.method public static int32 Main (string[] args) noinlining
{
	.maxstack 8
	.entrypoint
	.locals init ( valuetype ValueType V_0 )

	ldc.i8 1000000
	call valuetype ValueType valuetype ValueType::Method1 (int64)
	stloc 0

	ldloc 0
	ldfld int64 ValueType::a0
	ldc.i8 0
	bne.un failure

	ldloc 0
	ldfld int64 ValueType::a5
	ldc.i8 5
	bne.un failure

	ldloc 0
	ldfld int64 ValueType::a9
	ldc.i8 9
	bne.un failure

	ldc.i4 0
	ret

failure:
	ldstr "the value did not survive the recursion"
	call void class [mscorlib]System.Console::WriteLine (string)
	ldc.i4 1
	ret
}

}
