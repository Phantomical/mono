//
// ECMA-335 II.14.3 lists the underlying types an enum may have, and II.22.37
// row 33.h marks a basetype outside that list [ERROR]. A struct,
// System.Object, and float32 must raise TypeLoadException once the class is
// laid out. A conforming basetype (int32) must still load.
//

.assembly extern mscorlib
{
	.ver 2:0:0:0
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 ) // .z\V.4..
}
.assembly 'enum-invalid-basetype'
{
	.hash algorithm 0x00008004
	.ver	0:0:0:0
}
.module enum_invalid_basetype.exe

.namespace Example
{
	.class public sequential ansi sealed beforefieldinit PlainStruct
		extends [mscorlib]System.ValueType
	{
		.field public int32 data
	}

	.class public auto ansi sealed EnumOverStruct
		extends [mscorlib]System.Enum
	{
		.field public specialname rtspecialname valuetype Example.PlainStruct value__
	}

	.class public auto ansi sealed EnumOverObject
		extends [mscorlib]System.Enum
	{
		.field public specialname rtspecialname object value__
	}

	.class public auto ansi sealed EnumOverFloat
		extends [mscorlib]System.Enum
	{
		.field public specialname rtspecialname float32 value__
	}

	.class public auto ansi sealed EnumOverInt32
		extends [mscorlib]System.Enum
	{
		.field public specialname rtspecialname int32 value__
	}

	.class public auto ansi beforefieldinit Tests
		extends [mscorlib]System.Object
	{
		.method private static hidebysig
			default void layout_enum_over_struct () cil managed
		{
			.maxstack 1
			.locals init (valuetype Example.EnumOverStruct V_0)
			ldloca.s 0
			initobj Example.EnumOverStruct
			ret
		}

		.method private static hidebysig
			default void layout_enum_over_object () cil managed
		{
			.maxstack 1
			.locals init (valuetype Example.EnumOverObject V_0)
			ldloca.s 0
			initobj Example.EnumOverObject
			ret
		}

		.method private static hidebysig
			default void layout_enum_over_float () cil managed
		{
			.maxstack 1
			.locals init (valuetype Example.EnumOverFloat V_0)
			ldloca.s 0
			initobj Example.EnumOverFloat
			ret
		}

		.method private static hidebysig
			default void layout_enum_over_int32 () cil managed
		{
			.maxstack 1
			.locals init (valuetype Example.EnumOverInt32 V_0)
			ldloca.s 0
			initobj Example.EnumOverInt32
			ret
		}

		.method public static hidebysig default int32 test_0_enum_over_struct_rejected () cil managed
		{
			.maxstack 2
			.try {
				call void class Example.Tests::layout_enum_over_struct ()
				leave BAD
			}
			catch class [mscorlib]System.TypeLoadException {
				pop
				leave GOOD
			}
		BAD:
			ldc.i4.1
			ret
		GOOD:
			ldc.i4.0
			ret
		}

		.method public static hidebysig default int32 test_0_enum_over_object_rejected () cil managed
		{
			.maxstack 2
			.try {
				call void class Example.Tests::layout_enum_over_object ()
				leave BAD
			}
			catch class [mscorlib]System.TypeLoadException {
				pop
				leave GOOD
			}
		BAD:
			ldc.i4.1
			ret
		GOOD:
			ldc.i4.0
			ret
		}

		.method public static hidebysig default int32 test_0_enum_over_float_rejected () cil managed
		{
			.maxstack 2
			.try {
				call void class Example.Tests::layout_enum_over_float ()
				leave BAD
			}
			catch class [mscorlib]System.TypeLoadException {
				pop
				leave GOOD
			}
		BAD:
			ldc.i4.1
			ret
		GOOD:
			ldc.i4.0
			ret
		}

		.method public static hidebysig default int32 test_0_enum_over_int32_still_loads () cil managed
		{
			.maxstack 2
			.try {
				call void class Example.Tests::layout_enum_over_int32 ()
				leave GOOD
			}
			catch class [mscorlib]System.TypeLoadException {
				pop
				leave BAD
			}
		BAD:
			ldc.i4.1
			ret
		GOOD:
			ldc.i4.0
			ret
		}

		.method private static hidebysig
			default int32 Main (string[] args) cil managed
		{
			.entrypoint
			.maxstack 2
			.locals init (int32 V_0)

			ldc.i4.0
			stloc.0

			call int32 class Example.Tests::test_0_enum_over_struct_rejected ()
			ldloc.0
			add
			stloc.0

			call int32 class Example.Tests::test_0_enum_over_object_rejected ()
			ldloc.0
			add
			stloc.0

			call int32 class Example.Tests::test_0_enum_over_float_rejected ()
			ldloc.0
			add
			stloc.0

			call int32 class Example.Tests::test_0_enum_over_int32_still_loads ()
			ldloc.0
			add
			stloc.0

			ldloc.0
			ret
		}
	}
}
