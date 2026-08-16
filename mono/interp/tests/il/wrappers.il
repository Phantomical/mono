// Marshalling wrappers, run by the interpreter rather than compiled.
//
// A wrapper is normally compiled whatever tier its target runs in, so the
// opcodes only a wrapper carries -- mono_ldnativeobj, mono_retobj, mono_vtaddr,
// mono_lddomain and the rest -- never reach the transform. `interp-only=<class>`
// makes the runtime hand every method of a class to the interpreter, wrappers
// included, and the `wrappers/` arm passes it for this assembly.
//
// The callees are the runner's own, in native.cpp. What is under test is the
// wrapper the runtime builds around each signature, not the native function.

.assembly extern mscorlib { }
.assembly 'wrappers' { }

.class public sequential ansi sealed WrappersPair extends [mscorlib]System.ValueType
{
	.field public int32 quotient
	.field public int32 remainder
}

.class public auto ansi WrappersIL extends [mscorlib]System.Object
{
	.method public static pinvokeimpl("__Internal" as "interp_test_abs" cdecl)
	        int32 abs (int32 n) cil managed preservesig { }

	.method public static pinvokeimpl("__Internal" as "interp_test_labs" cdecl)
	        int64 llabs (int64 n) cil managed preservesig { }

	.method public static pinvokeimpl("__Internal" as "interp_test_atof" cdecl)
	        float64 atof (string text) cil managed preservesig { }

	.method public static pinvokeimpl("__Internal" as "interp_test_strlen" cdecl)
	        native int strlen (string text) cil managed preservesig { }

	.method public static pinvokeimpl("__Internal" as "interp_test_div" cdecl)
	        valuetype WrappersPair divide (int32 numerator, int32 denominator) cil managed preservesig { }

	.method public static pinvokeimpl("__Internal" as "interp_test_memcmp" cdecl)
	        int32 memcmp (int8[] first, int8[] second, native int count) cil managed preservesig { }

	.method public static pinvokeimpl("__Internal" as "interp_test_free" cdecl)
	        void free (native int address) cil managed preservesig { }

	.method public static int32 test_7_native_int_argument () cil managed
	{
		.maxstack 2
		ldc.i4.s -7
		call int32 WrappersIL::abs (int32)
		ret
	}

	.method public static int32 test_1_native_long_argument () cil managed
	{
		.maxstack 2
		ldc.i8 -4294967297
		call int64 WrappersIL::llabs (int64)
		ldc.i8 4294967297
		beq IL_ok
		ldc.i4.0
		ret
	IL_ok:
		ldc.i4.1
		ret
	}

	.method public static int32 test_1_native_float_return () cil managed
	{
		.maxstack 2
		ldstr "2.5"
		call float64 WrappersIL::atof (string)
		ldc.r8 2.5
		beq IL_ok
		ldc.i4.0
		ret
	IL_ok:
		ldc.i4.1
		ret
	}

	.method public static int32 test_5_native_string_argument () cil managed
	{
		.maxstack 2
		ldstr "hello"
		call native int WrappersIL::strlen (string)
		conv.i4
		ret
	}

	// A struct return, which is what mono_retobj and mono_vtaddr are for.
	.method public static int32 test_4_native_struct_return () cil managed
	{
		.maxstack 3
		.locals init (valuetype WrappersPair p)
		ldc.i4.7
		ldc.i4.2
		call valuetype WrappersPair WrappersIL::divide (int32, int32)
		stloc.0
		ldloc.0
		ldfld int32 WrappersPair::quotient
		ldloc.0
		ldfld int32 WrappersPair::remainder
		add
		ret
	}

	.method public static int32 test_1_native_array_arguments () cil managed
	{
		.maxstack 4
		.locals init (int8[] a, int8[] b)
		ldc.i4.3
		newarr [mscorlib]System.SByte
		stloc.0
		ldc.i4.3
		newarr [mscorlib]System.SByte
		stloc.1
		ldloc.0
		ldc.i4.0
		ldc.i4.1
		stelem.i1
		ldloc.1
		ldc.i4.0
		ldc.i4.1
		stelem.i1
		ldloc.0
		ldloc.1
		ldc.i4.3
		conv.i
		call int32 WrappersIL::memcmp (int8[], int8[], native int)
		brtrue IL_differ
		ldc.i4.1
		ret
	IL_differ:
		ldc.i4.0
		ret
	}

	// A void return, so the wrapper has nothing to hand back.
	.method public static int32 test_3_native_void_return () cil managed
	{
		.maxstack 2
		ldc.i4.0
		conv.i
		call void WrappersIL::free (native int)
		ldc.i4.3
		ret
	}
}
