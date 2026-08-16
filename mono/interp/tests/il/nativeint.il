// Native-int operands.  A C# compiler widens an int32 before it reaches a
// native-int location; IL does not have to, and the interpreter and the
// translator disagree about what happens when it does not.

.assembly extern mscorlib { }

.class public auto ansi NativeInt extends [mscorlib]System.Object
{
	.field public static native int slot
	.field public static native unsigned int uslot
	.field public native int held

	.method public specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ret
	}

	// An int32 on the stack stored straight into a native int static.  The
	// destination is eight bytes wide and the value four.
	.method public static int32 test_5_stsfld_widens () cil managed
	{
		.maxstack 2
		ldc.i4 5
		stsfld native int NativeInt::slot
		ldsfld native int NativeInt::slot
		conv.i8
		ldc.i8 5
		beq IL_ok
		ldc.i4.0
		ret
	IL_ok:
		ldc.i4.5
		ret
	}

	// The same store through a pointer rather than to a field.
	.method public static int32 test_5_stind_i_widens () cil managed
	{
		.maxstack 3
		.locals init (native int held)
		ldc.i8 0x7FFFFFFF00000000
		conv.i
		stloc.0
		ldloca.s 0
		ldc.i4 5
		stind.i
		ldloc.0
		conv.i8
		ldc.i8 5
		beq IL_ok
		ldc.i4.0
		ret
	IL_ok:
		ldc.i4.5
		ret
	}

	// A negative int32 says which way the widening goes: ECMA-335 I.8.7 makes
	// this a sign extension, so the native int reads back as -1.
	.method public static int32 test_1_stind_i_sign_extends () cil managed
	{
		.maxstack 3
		.locals init (native int held)
		ldloca.s 0
		ldc.i4.m1
		stind.i
		ldloc.0
		conv.i8
		ldc.i8 -1
		beq IL_ok
		ldc.i4.0
		ret
	IL_ok:
		ldc.i4.1
		ret
	}

	// An array index whose high half is set.  Truncating it to 32 bits selects
	// element 0 of a ten-element array instead of raising IndexOutOfRange.
	.method public static int32 test_1_wide_array_index_throws () cil managed
	{
		.maxstack 3
		.locals init (int32[] a)
		.try {
			ldc.i4 10
			newarr [mscorlib]System.Int32
			stloc.0
			ldloc.0
			ldc.i8 0x100000000
			conv.i
			ldelem.i4
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.IndexOutOfRangeException {
			pop
			ldc.i4.1
			ret
		}
	}

	// The same high half on a switch operand.  Every arm is out of range, so
	// control has to fall through to the default.
	.method public static int32 test_7_wide_switch_index_falls_through () cil managed
	{
		.maxstack 2
		ldc.i8 0x100000000
		conv.i
		switch (IL_zero, IL_one)
		ldc.i4.7
		ret
	IL_zero:
		ldc.i4.0
		ret
	IL_one:
		ldc.i4.1
		ret
	}

	// The other places a value reaches a native int location.  A negative
	// source says which way the widening goes whatever the stack slot held
	// before it: a sign extension always reads back as -1.

	.method public static int32 test_1_stfld_sign_extends () cil managed
	{
		.maxstack 3
		newobj instance void NativeInt::.ctor ()
		dup
		ldc.i4.m1
		stfld native int NativeInt::held
		ldfld native int NativeInt::held
		conv.i8
		ldc.i8 -1
		ceq
		ret
	}

	.method public static int32 test_1_stelem_i_sign_extends () cil managed
	{
		.maxstack 4
		ldc.i4.1
		newarr [mscorlib]System.IntPtr
		dup
		ldc.i4.0
		ldc.i4.m1
		stelem.i
		ldc.i4.0
		ldelem.i
		conv.i8
		ldc.i8 -1
		ceq
		ret
	}

	.method public static native int NarrowReturn () cil managed noinlining
	{
		.maxstack 1
		ldc.i4.m1
		ret
	}

	.method public static int32 test_1_ret_sign_extends () cil managed
	{
		.maxstack 3
		call native int NativeInt::NarrowReturn ()
		conv.i8
		ldc.i8 -1
		ceq
		ret
	}

	.method public static int32 TakesNative (native int given) cil managed noinlining
	{
		.maxstack 3
		ldarg.0
		conv.i8
		ldc.i8 -1
		ceq
		ret
	}

	.method public static int32 test_1_call_argument_sign_extends () cil managed
	{
		.maxstack 1
		ldc.i4.m1
		call int32 NativeInt::TakesNative (native int)
		ret
	}

	// The index side at the two array opcodes the tests above do not reach.

	.method public static int32 test_1_wide_stelem_index_throws () cil managed
	{
		.maxstack 4
		.locals init (int32 result)
		.try {
			ldc.i4 10
			newarr [mscorlib]System.Int32
			ldc.i8 0x100000000
			conv.i
			ldc.i4.5
			stelem.i4
			leave IL_done
		} catch [mscorlib]System.IndexOutOfRangeException {
			pop
			ldc.i4.1
			stloc.0
			leave IL_done
		}
	IL_done:
		ldloc.0
		ret
	}

	.method public static int32 test_1_wide_ldelema_index_throws () cil managed
	{
		.maxstack 3
		.locals init (int32 result)
		.try {
			ldc.i4 10
			newarr [mscorlib]System.Int32
			ldc.i8 0x100000000
			conv.i
			ldelema [mscorlib]System.Int32
			pop
			leave IL_done
		} catch [mscorlib]System.IndexOutOfRangeException {
			pop
			ldc.i4.1
			stloc.0
			leave IL_done
		}
	IL_done:
		ldloc.0
		ret
	}

	// A native int index that does fit still reads its element, and a negative
	// one still throws.  Narrowing the wide ones must not cost either.

	.method public static int32 test_3_native_int_index_reads () cil managed
	{
		.maxstack 4
		.locals init (int32[] a)
		ldc.i4 10
		newarr [mscorlib]System.Int32
		stloc.0
		ldloc.0
		ldc.i4.2
		ldc.i4.3
		stelem.i4
		ldloc.0
		ldc.i8 2
		conv.i
		ldelem.i4
		ret
	}

	.method public static int32 test_1_negative_native_int_index_throws () cil managed
	{
		.maxstack 3
		.locals init (int32 result)
		.try {
			ldc.i4 10
			newarr [mscorlib]System.Int32
			ldc.i8 -1
			conv.i
			ldelem.i4
			pop
			leave IL_done
		} catch [mscorlib]System.IndexOutOfRangeException {
			pop
			ldc.i4.1
			stloc.0
			leave IL_done
		}
	IL_done:
		ldloc.0
		ret
	}

	// A native unsigned int location takes zeroes rather than the sign, so -1
	// reaches it as 0xFFFFFFFF.  ECMA-335 Table III.9 splits the two.

	.method public static int32 test_1_stsfld_zero_extends () cil managed
	{
		.maxstack 2
		ldc.i4.m1
		stsfld native unsigned int NativeInt::uslot
		ldsfld native unsigned int NativeInt::uslot
		conv.i8
		ldc.i8 0xFFFFFFFF
		ceq
		ret
	}

	.method public static int32 TakesNativeUnsigned (native unsigned int given) cil managed noinlining
	{
		.maxstack 3
		ldarg.0
		conv.i8
		ldc.i8 0xFFFFFFFF
		ceq
		ret
	}

	.method public static int32 test_1_call_argument_zero_extends () cil managed
	{
		.maxstack 1
		ldc.i4.m1
		call int32 NativeInt::TakesNativeUnsigned (native unsigned int)
		ret
	}

	.method public static native unsigned int NarrowUnsignedReturn () cil managed noinlining
	{
		.maxstack 1
		ldc.i4.m1
		ret
	}

	.method public static int32 test_1_ret_zero_extends () cil managed
	{
		.maxstack 3
		call native unsigned int NativeInt::NarrowUnsignedReturn ()
		conv.i8
		ldc.i8 0xFFFFFFFF
		ceq
		ret
	}

	.method public static int32 test_1_native_int_switch_selects () cil managed
	{
		.maxstack 2
		ldc.i8 1
		conv.i
		switch (IL_zero, IL_one)
		ldc.i4.7
		ret
	IL_zero:
		ldc.i4.0
		ret
	IL_one:
		ldc.i4.1
		ret
	}
}
