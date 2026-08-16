// Native-int operands.  A C# compiler widens an int32 before it reaches a
// native-int location; IL does not have to, and the interpreter and the
// translator disagree about what happens when it does not.

.assembly extern mscorlib { }
.assembly 'nativeint' { }

.class public auto ansi NativeInt extends [mscorlib]System.Object
{
	.field public static native int slot

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
}
