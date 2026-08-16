// ldelem and stelem with a type token, one test for each arm of the switch that
// picks the opcode out of mint_type (). C# names a primitive element with
// ldelem.i4 and its siblings, so an explicit token is the only way to name a
// narrow width outside a generic method.
//
// The Get, Set and Address methods of a single-dimensional array are here too.
// The transform sends those to interp_emit_ldelema (), whose rank-1 path a
// rank-2 array never takes.
//
// elements.cs reaches the same mapping through a generic element type,
// arrays-il.il holds the multi-dimensional forms, and arrays.cs holds the
// bounds, null and covariance checks these opcodes share with the plain forms.
//
// Indices and values come out of a noinlining helper, so the transform cannot
// fold them away.

.assembly extern mscorlib { }
.assembly 'elements-il' { }

.class public sequential ansi ElementsILPair extends [mscorlib]System.ValueType
{
	.field public int32 first
	.field public int32 second
}

.class public sequential ansi ElementsILTiny extends [mscorlib]System.ValueType
{
	.field public int8 only
}

.class public sequential ansi ElementsILHolder`1<T> extends [mscorlib]System.ValueType
{
	.field public !T held
}

.class public auto ansi ElementsILBox`1<T> extends [mscorlib]System.Object
{
	.field public !T held

	.method public specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ret
	}
}

.class public auto ansi sealed ElementsILSByteEnum extends [mscorlib]System.Enum
{
	.field public specialname rtspecialname int8 value__
}

.class public auto ansi sealed ElementsILByteEnum extends [mscorlib]System.Enum
{
	.field public specialname rtspecialname unsigned int8 value__
}

.class public auto ansi sealed ElementsILShortEnum extends [mscorlib]System.Enum
{
	.field public specialname rtspecialname int16 value__
}

.class public auto ansi sealed ElementsILUShortEnum extends [mscorlib]System.Enum
{
	.field public specialname rtspecialname unsigned int16 value__
}

.class public auto ansi sealed ElementsILIntEnum extends [mscorlib]System.Enum
{
	.field public specialname rtspecialname int32 value__
}

.class public auto ansi sealed ElementsILLongEnum extends [mscorlib]System.Enum
{
	.field public specialname rtspecialname int64 value__
}

.class public auto ansi ElementsIL extends [mscorlib]System.Object
{
	.method public static int32 Id (int32 x) cil managed noinlining
	{
		.maxstack 1
		ldarg.0
		ret
	}

	.method public static int64 IdL (int64 x) cil managed noinlining
	{
		.maxstack 1
		ldarg.0
		ret
	}

	.method public static float64 IdR8 (float64 x) cil managed noinlining
	{
		.maxstack 1
		ldarg.0
		ret
	}

	// ------------------------------------------------------------------
	// One integer width per test, signed and unsigned
	// ------------------------------------------------------------------

	// The element holds one byte and the load sign-extends it, so an unsigned
	// load would give 255 here.
	.method public static int32 test_1_int8_token () cil managed
	{
		.maxstack 4
		.locals init (int8[] a)
		ldc.i4.2
		newarr int8
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldc.i4.m1
		call int32 ElementsIL::Id (int32)
		stelem int8
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem int8
		ldc.i4.m1
		ceq
		ret
	}

	// The value keeps its low byte, and the load zero-extends it.
	.method public static int32 test_255_unsigned_int8_token () cil managed
	{
		.maxstack 4
		.locals init (unsigned int8[] a)
		ldc.i4.2
		newarr unsigned int8
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldc.i4 511
		call int32 ElementsIL::Id (int32)
		stelem unsigned int8
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem unsigned int8
		ret
	}

	// A bool element is one byte wide. The neighbour stays zero, which is what a
	// store of the whole four bytes would break.
	.method public static int32 test_1_bool_token () cil managed
	{
		.maxstack 4
		.locals init (bool[] a)
		ldc.i4.2
		newarr bool
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldc.i4 513
		call int32 ElementsIL::Id (int32)
		stelem bool
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelem bool
		ldc.i4.1
		ceq
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem bool
		ldc.i4.0
		ceq
		and
		ret
	}

	.method public static int32 test_1_int16_token () cil managed
	{
		.maxstack 4
		.locals init (int16[] a)
		ldc.i4.2
		newarr int16
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldc.i4.m1
		call int32 ElementsIL::Id (int32)
		stelem int16
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelem int16
		ldc.i4.m1
		ceq
		ret
	}

	.method public static int32 test_65535_unsigned_int16_token () cil managed
	{
		.maxstack 4
		.locals init (unsigned int16[] a)
		ldc.i4.2
		newarr unsigned int16
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldc.i4.m1
		call int32 ElementsIL::Id (int32)
		stelem unsigned int16
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelem unsigned int16
		ret
	}

	.method public static int32 test_65535_char_token () cil managed
	{
		.maxstack 4
		.locals init (char[] a)
		ldc.i4.2
		newarr char
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldc.i4.m1
		call int32 ElementsIL::Id (int32)
		stelem char
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem char
		ret
	}

	.method public static int32 test_7_int32_token () cil managed
	{
		.maxstack 4
		.locals init (int32[] a)
		ldc.i4.3
		newarr int32
		stloc.0
		ldloc.0
		ldc.i4.2
		call int32 ElementsIL::Id (int32)
		ldc.i4.7
		call int32 ElementsIL::Id (int32)
		stelem int32
		ldloc.0
		ldc.i4.2
		call int32 ElementsIL::Id (int32)
		ldelem int32
		ret
	}

	.method public static int32 test_1_unsigned_int32_token () cil managed
	{
		.maxstack 4
		.locals init (unsigned int32[] a)
		ldc.i4.2
		newarr unsigned int32
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldc.i4.m1
		call int32 ElementsIL::Id (int32)
		stelem unsigned int32
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelem unsigned int32
		ldc.i4.m1
		ceq
		ret
	}

	.method public static int32 test_1_int64_token () cil managed
	{
		.maxstack 4
		.locals init (int64[] a)
		ldc.i4.2
		newarr int64
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldc.i8 0x1122334455667788
		call int64 ElementsIL::IdL (int64)
		stelem int64
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem int64
		ldc.i8 0x1122334455667788
		ceq
		ret
	}

	.method public static int32 test_1_unsigned_int64_token () cil managed
	{
		.maxstack 4
		.locals init (unsigned int64[] a)
		ldc.i4.2
		newarr unsigned int64
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldc.i8 -1
		call int64 ElementsIL::IdL (int64)
		stelem unsigned int64
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelem unsigned int64
		ldc.i8 -1
		ceq
		ret
	}

	// An array of pointers. mint_type () answers a MONO_TYPE_PTR with the
	// pointer-wide arm, and a generic element type cannot name one.
	.method public static int32 test_9_pointer_token () cil managed
	{
		.maxstack 4
		.locals init (int32*[] a, int32 held)
		ldc.i4.s 9
		call int32 ElementsIL::Id (int32)
		stloc.1
		ldc.i4.2
		newarr int32*
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldloca.s 1
		conv.u
		stelem int32*
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelem int32*
		ldind.i4
		ret
	}

	// ------------------------------------------------------------------
	// Floating point
	// ------------------------------------------------------------------

	// 0.1 has no exact float. The element equals the single and differs from the
	// double, so the store kept four bytes of it.
	.method public static int32 test_1_float32_token () cil managed
	{
		.maxstack 4
		.locals init (float32[] a)
		ldc.i4.2
		newarr float32
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldc.r8 0.1
		call float64 ElementsIL::IdR8 (float64)
		conv.r4
		stelem float32
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem float32
		ldc.r4 0.1
		ceq
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem float32
		conv.r8
		ldc.r8 0.1
		ceq
		ldc.i4.0
		ceq
		and
		ret
	}

	.method public static int32 test_3_float64_token () cil managed
	{
		.maxstack 4
		.locals init (float64[] a)
		ldc.i4.2
		newarr float64
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldc.r8 1.5
		call float64 ElementsIL::IdR8 (float64)
		stelem float64
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelem float64
		ldc.r8 1.5
		add
		conv.i4
		ret
	}

	// ------------------------------------------------------------------
	// References
	// ------------------------------------------------------------------

	.method public static int32 test_5_object_token () cil managed
	{
		.maxstack 4
		.locals init (object[] a)
		ldc.i4.2
		newarr [mscorlib]System.Object
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldc.i4.5
		call int32 ElementsIL::Id (int32)
		box [mscorlib]System.Int32
		stelem [mscorlib]System.Object
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem [mscorlib]System.Object
		unbox.any [mscorlib]System.Int32
		ret
	}

	// An array element type, which mint_type () also answers with the reference
	// arm. The row is longer than the array that holds it, so a load of the
	// wrong element cannot give 3.
	.method public static int32 test_3_array_element_token () cil managed
	{
		.maxstack 4
		.locals init (int32[][] a)
		ldc.i4.2
		newarr int32[]
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldc.i4.3
		newarr int32
		stelem int32[]
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem int32[]
		ldlen
		conv.i4
		ret
	}

	.method public static int32 test_6_generic_class_token () cil managed
	{
		.maxstack 4
		.locals init (class ElementsILBox`1<int32>[] a, class ElementsILBox`1<int32> one)
		newobj instance void class ElementsILBox`1<int32>::.ctor ()
		stloc.1
		ldloc.1
		ldc.i4.6
		call int32 ElementsIL::Id (int32)
		stfld !0 class ElementsILBox`1<int32>::held
		ldc.i4.2
		newarr class ElementsILBox`1<int32>
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldloc.1
		stelem class ElementsILBox`1<int32>
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelem class ElementsILBox`1<int32>
		ldfld !0 class ElementsILBox`1<int32>::held
		ret
	}

	// ------------------------------------------------------------------
	// Value types, which take the MINT_TYPE_VT arm
	// ------------------------------------------------------------------

	.method public static int32 test_9_value_type_token () cil managed
	{
		.maxstack 4
		.locals init (valuetype ElementsILPair[] a, valuetype ElementsILPair p)
		ldloca.s 1
		ldc.i4.4
		call int32 ElementsIL::Id (int32)
		stfld int32 ElementsILPair::first
		ldloca.s 1
		ldc.i4.5
		call int32 ElementsIL::Id (int32)
		stfld int32 ElementsILPair::second
		ldc.i4.2
		newarr ElementsILPair
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldloc.1
		stelem ElementsILPair
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem ElementsILPair
		stloc.1
		ldloc.1
		ldfld int32 ElementsILPair::first
		ldloc.1
		ldfld int32 ElementsILPair::second
		add
		ret
	}

	// A one byte value type, which is still the value type arm rather than the
	// narrow integer one.
	.method public static int32 test_7_one_byte_value_type_token () cil managed
	{
		.maxstack 4
		.locals init (valuetype ElementsILTiny[] a, valuetype ElementsILTiny t)
		ldloca.s 1
		ldc.i4.7
		call int32 ElementsIL::Id (int32)
		stfld int8 ElementsILTiny::only
		ldc.i4.3
		newarr ElementsILTiny
		stloc.0
		ldloc.0
		ldc.i4.2
		call int32 ElementsIL::Id (int32)
		ldloc.1
		stelem ElementsILTiny
		ldloc.0
		ldc.i4.2
		call int32 ElementsIL::Id (int32)
		ldelem ElementsILTiny
		stloc.1
		ldloc.1
		ldfld int8 ElementsILTiny::only
		ret
	}

	// A generic instance reaches mint_type () through its container class, which
	// is a different path from a plain value type.
	.method public static int32 test_3_generic_value_type_token () cil managed
	{
		.maxstack 4
		.locals init (valuetype ElementsILHolder`1<int32>[] a,
		              valuetype ElementsILHolder`1<int32> h)
		ldloca.s 1
		ldc.i4.3
		call int32 ElementsIL::Id (int32)
		stfld !0 valuetype ElementsILHolder`1<int32>::held
		ldc.i4.2
		newarr valuetype ElementsILHolder`1<int32>
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldloc.1
		stelem valuetype ElementsILHolder`1<int32>
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelem valuetype ElementsILHolder`1<int32>
		stloc.1
		ldloc.1
		ldfld !0 valuetype ElementsILHolder`1<int32>::held
		ret
	}

	// ------------------------------------------------------------------
	// Enums, which mint_type () answers with the underlying width
	// ------------------------------------------------------------------

	.method public static int32 test_1_sbyte_enum_token () cil managed
	{
		.maxstack 4
		.locals init (valuetype ElementsILSByteEnum[] a)
		ldc.i4.2
		newarr ElementsILSByteEnum
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldc.i4.m1
		call int32 ElementsIL::Id (int32)
		stelem ElementsILSByteEnum
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem ElementsILSByteEnum
		ldc.i4.m1
		ceq
		ret
	}

	.method public static int32 test_255_byte_enum_token () cil managed
	{
		.maxstack 4
		.locals init (valuetype ElementsILByteEnum[] a)
		ldc.i4.2
		newarr ElementsILByteEnum
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldc.i4 255
		call int32 ElementsIL::Id (int32)
		stelem ElementsILByteEnum
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelem ElementsILByteEnum
		ret
	}

	.method public static int32 test_1_short_enum_token () cil managed
	{
		.maxstack 4
		.locals init (valuetype ElementsILShortEnum[] a)
		ldc.i4.2
		newarr ElementsILShortEnum
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldc.i4.m1
		call int32 ElementsIL::Id (int32)
		stelem ElementsILShortEnum
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem ElementsILShortEnum
		ldc.i4.m1
		ceq
		ret
	}

	.method public static int32 test_65535_ushort_enum_token () cil managed
	{
		.maxstack 4
		.locals init (valuetype ElementsILUShortEnum[] a)
		ldc.i4.2
		newarr ElementsILUShortEnum
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldc.i4.m1
		call int32 ElementsIL::Id (int32)
		stelem ElementsILUShortEnum
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelem ElementsILUShortEnum
		ret
	}

	.method public static int32 test_9_int_enum_token () cil managed
	{
		.maxstack 4
		.locals init (valuetype ElementsILIntEnum[] a)
		ldc.i4.2
		newarr ElementsILIntEnum
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldc.i4.s 9
		call int32 ElementsIL::Id (int32)
		stelem ElementsILIntEnum
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem ElementsILIntEnum
		ret
	}

	.method public static int32 test_1_long_enum_token () cil managed
	{
		.maxstack 4
		.locals init (valuetype ElementsILLongEnum[] a)
		ldc.i4.2
		newarr ElementsILLongEnum
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldc.i8 0x1122334455667788
		call int64 ElementsIL::IdL (int64)
		stelem ElementsILLongEnum
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelem ElementsILLongEnum
		ldc.i8 0x1122334455667788
		ceq
		ret
	}

	// ------------------------------------------------------------------
	// Get, Set and Address on a single-dimensional array
	// ------------------------------------------------------------------

	// These methods exist on every array class, rank one included, so the IL can
	// name them where C# would have written an element access.
	.method public static int32 test_6_szarray_set_and_get () cil managed
	{
		.maxstack 4
		.locals init (int32[] a)
		ldc.i4.3
		newarr int32
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldc.i4.6
		call int32 ElementsIL::Id (int32)
		call instance void int32[]::Set (int32, int32)
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		call instance int32 int32[]::Get (int32)
		ret
	}

	.method public static int32 test_9_szarray_address () cil managed
	{
		.maxstack 4
		.locals init (int32[] a)
		ldc.i4.2
		newarr int32
		stloc.0
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		call instance int32& int32[]::Address (int32)
		ldc.i4.s 9
		call int32 ElementsIL::Id (int32)
		stind.i4
		ldloc.0
		ldc.i4.1
		call int32 ElementsIL::Id (int32)
		ldelem int32
		ret
	}

	// The element size the rank-1 path carries is the size of the struct here,
	// rather than the four bytes a primitive gives it.
	.method public static int32 test_4_szarray_address_of_a_value_type () cil managed
	{
		.maxstack 4
		.locals init (valuetype ElementsILPair[] a)
		ldc.i4.2
		newarr ElementsILPair
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		call instance valuetype ElementsILPair& ElementsILPair[]::Address (int32)
		ldc.i4.4
		call int32 ElementsIL::Id (int32)
		stfld int32 ElementsILPair::first
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldelema ElementsILPair
		ldfld int32 ElementsILPair::first
		ret
	}

	// readonly. drops the type check, so the address of a reference element comes
	// back through the same rank-1 path a value type takes.
	.method public static int32 test_4_readonly_szarray_address () cil managed
	{
		.maxstack 4
		.locals init (object[] a)
		ldc.i4.2
		newarr [mscorlib]System.String
		stloc.0
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		ldstr "kept"
		call instance void object[]::Set (int32, object)
		ldloc.0
		ldc.i4.0
		call int32 ElementsIL::Id (int32)
		readonly.
		call instance object& object[]::Address (int32)
		ldind.ref
		castclass [mscorlib]System.String
		callvirt instance int32 [mscorlib]System.String::get_Length ()
		ret
	}

	// Set takes its check class from the value it was given rather than from the
	// array, which no other caller of the rank-1 path does.
	.method public static int32 test_1_szarray_set_is_type_checked () cil managed
	{
		.maxstack 4
		.locals init (object[] a)
		.try {
			ldc.i4.2
			newarr [mscorlib]System.String
			stloc.0
			ldloc.0
			ldc.i4.0
			call int32 ElementsIL::Id (int32)
			ldc.i4.5
			call int32 ElementsIL::Id (int32)
			box [mscorlib]System.Int32
			call instance void object[]::Set (int32, object)
			ldc.i4.0
			ret
		} catch [mscorlib]System.ArrayTypeMismatchException {
			pop
			ldc.i4.1
			ret
		}
	}
}
