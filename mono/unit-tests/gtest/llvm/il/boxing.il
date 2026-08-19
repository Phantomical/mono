// Boxing and unboxing: box allocates and copies, unbox type-checks and hands out an
// interior pointer, unbox.any loads the value back out.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'boxing' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'boxing.dll'

.class public sequential ansi sealed beforefieldinit Pair extends [mscorlib]System.ValueType
{
	.field public int32 A
	.field public int64 B
}

.class public sequential ansi sealed beforefieldinit RefPair extends [mscorlib]System.ValueType
{
	.field public object O
	.field public int32 X
}

.class public abstract auto ansi sealed beforefieldinit Boxing extends [mscorlib]System.Object
{
	.method public static object BoxInt (int32 v) cil managed
	{
		.maxstack 1
		ldarg.0
		box [mscorlib]System.Int32
		ret
	}

	.method public static object BoxPair (valuetype Pair p) cil managed
	{
		.maxstack 1
		ldarg.0
		box Pair
		ret
	}

	.method public static object BoxRefPair (valuetype RefPair p) cil managed
	{
		.maxstack 1
		ldarg.0
		box RefPair
		ret
	}

	.method public static object BoxObject (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		box [mscorlib]System.Object
		ret
	}

	.method public static int32 UnboxInt (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		unbox [mscorlib]System.Int32
		ldind.i4
		ret
	}

	.method public static int32 UnboxAnyInt (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		unbox.any [mscorlib]System.Int32
		ret
	}

	.method public static valuetype Pair UnboxAnyPair (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		unbox.any Pair
		ret
	}

	.method public static int32 RoundTrip (int32 v) cil managed
	{
		.maxstack 1
		ldarg.0
		box [mscorlib]System.Int32
		unbox.any [mscorlib]System.Int32
		ret
	}

	.method public static object BoxNullable (valuetype [mscorlib]System.Nullable`1<int32> v) cil managed
	{
		.maxstack 1
		ldarg.0
		box valuetype [mscorlib]System.Nullable`1<int32>
		ret
	}

	.method public static valuetype [mscorlib]System.Nullable`1<int32> UnboxNullable (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		unbox valuetype [mscorlib]System.Nullable`1<int32>
		ldobj valuetype [mscorlib]System.Nullable`1<int32>
		ret
	}

	.method public static valuetype [mscorlib]System.Nullable`1<int32> UnboxAnyNullable (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		unbox.any valuetype [mscorlib]System.Nullable`1<int32>
		ret
	}

	.method public static valuetype [mscorlib]System.Nullable`1<valuetype Color> UnboxAnyNullableEnum (object o) cil managed
	{
		.maxstack 1
		ldarg.0
		unbox.any valuetype [mscorlib]System.Nullable`1<valuetype Color>
		ret
	}
}

.class public auto ansi sealed Color extends [mscorlib]System.Enum
{
	.field public specialname rtspecialname int32 value__
	.field public static literal valuetype Color Red = int32(0)
}
