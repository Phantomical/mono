// Instance and static field access, including the reference store that has to go
// through the write barrier.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'fields' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'fields.dll'

.class public auto ansi beforefieldinit Holder extends [mscorlib]System.Object
{
	.field public int32 X
	.field public int64 Y
	.field public int8 Small
	.field public object Ref
	.field public static int32 S
	.field public static object SRef
	.field public static int32 PerThread
	.custom instance void [mscorlib]System.ThreadStaticAttribute::.ctor () = (01 00 00 00)
	.field public static int32 Baked at D_BAKED
	.field public valuetype Mixed M
	.field public static valuetype Mixed SM
	.field public valuetype Flat F

	.method public hidebysig specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ret
	}
}

.class public abstract auto ansi sealed beforefieldinit Fields extends [mscorlib]System.Object
{
	.method public static int32 Deref (int32& p) cil managed
	{
		.maxstack 1
		ldc.i4.0
		ret
	}

	.method public static int32 GetX (class Holder h) cil managed
	{
		.maxstack 1
		ldarg.0
		ldfld int32 Holder::X
		ret
	}

	.method public static int64 GetY (class Holder h) cil managed
	{
		.maxstack 1
		ldarg.0
		ldfld int64 Holder::Y
		ret
	}

	.method public static int32 GetSmall (class Holder h) cil managed
	{
		.maxstack 1
		ldarg.0
		ldfld int8 Holder::Small
		ret
	}

	.method public static void SetX (class Holder h, int32 v) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		stfld int32 Holder::X
		ret
	}

	.method public static void SetRef (class Holder h, object v) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		stfld object Holder::Ref
		ret
	}

	.method public static int32 FieldAddress (class Holder h) cil managed
	{
		.maxstack 1
		ldarg.0
		ldflda int32 Holder::X
		call int32 Fields::Deref (int32&)
		ret
	}

	.method public static int32 GetStatic () cil managed
	{
		.maxstack 1
		ldsfld int32 Holder::S
		ret
	}

	.method public static void SetStatic (int32 v) cil managed
	{
		.maxstack 1
		ldarg.0
		stsfld int32 Holder::S
		ret
	}

	.method public static void SetStaticRef (object v) cil managed
	{
		.maxstack 1
		ldarg.0
		stsfld object Holder::SRef
		ret
	}

	.method public static int32 StaticAddress () cil managed
	{
		.maxstack 1
		ldsflda int32 Holder::S
		call int32 Fields::Deref (int32&)
		ret
	}

	// Two statics of the same class, so the emitted IR can be checked for one
	// shared block symbol rather than one symbol per field.
	.method public static int32 TwoStatics () cil managed
	{
		.maxstack 2
		ldsfld int32 Holder::S
		ldsfld int32 Holder::S
		add
		ret
	}

	.method public static int32 GetPerThread () cil managed
	{
		.maxstack 1
		ldsfld int32 Holder::PerThread
		ret
	}

	.method public static void SetPerThread (int32 v) cil managed
	{
		.maxstack 1
		ldarg.0
		stsfld int32 Holder::PerThread
		ret
	}

	.method public static int32 GetBaked () cil managed
	{
		.maxstack 1
		ldsfld int32 Holder::Baked
		ret
	}

	.method public static void SetMixed (class Holder h, valuetype Mixed v) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		stfld valuetype Mixed Holder::M
		ret
	}

	.method public static void SetStaticMixed (valuetype Mixed v) cil managed
	{
		.maxstack 1
		ldarg.0
		stsfld valuetype Mixed Holder::SM
		ret
	}

	.method public static void SetFlat (class Holder h, valuetype Flat v) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		stfld valuetype Flat Holder::F
		ret
	}
}

// A struct the collector cares about - its stores go through the value-copy
// barrier - and one it does not.
.class public sequential ansi sealed beforefieldinit Mixed extends [mscorlib]System.ValueType
{
	.field public object Ref
	.field public int32 Tag
}

.class public sequential ansi sealed beforefieldinit Flat extends [mscorlib]System.ValueType
{
	.field public int32 A
	.field public int32 B
}

.data D_BAKED = int32 (42)
