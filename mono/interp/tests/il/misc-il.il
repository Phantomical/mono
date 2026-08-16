// Opcodes and prefixes with no C# spelling, and the IL shapes a compiler has no
// reason to emit.

.assembly extern mscorlib { }
.assembly 'misc-il' { }

.class public sequential ansi MiscPoint extends [mscorlib]System.ValueType
{
	.field public int32 x
	.field public int32 y
}

.class public auto ansi MiscIL extends [mscorlib]System.Object
{
	.field public static int32 counter

	.method public static int32 test_1_ckfinite_passes () cil managed
	{
		.maxstack 2
		ldc.r8 1.5
		ckfinite
		ldc.r8 1.5
		beq IL_ok
		ldc.i4.0
		ret
	IL_ok:
		ldc.i4.1
		ret
	}

	.method public static int32 test_1_ckfinite_nan_throws () cil managed
	{
		.maxstack 2
		.try {
			ldc.r8 0.0
			ldc.r8 0.0
			div
			ckfinite
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.ArithmeticException {
			pop
			ldc.i4.1
			ret
		}
	}

	.method public static int32 test_1_ckfinite_infinity_throws () cil managed
	{
		.maxstack 2
		.try {
			ldc.r8 1.0
			ldc.r8 0.0
			div
			ckfinite
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.ArithmeticException {
			pop
			ldc.i4.1
			ret
		}
	}

	// dup on a value type leaves two copies of the whole struct.
	.method public static int32 test_10_dup_valuetype () cil managed
	{
		.maxstack 4
		.locals init (valuetype MiscPoint p, valuetype MiscPoint q)
		ldloca.s 0
		ldc.i4.4
		stfld int32 MiscPoint::x
		ldloca.s 0
		ldc.i4.6
		stfld int32 MiscPoint::y
		ldloc.0
		dup
		stloc.1
		ldfld int32 MiscPoint::x
		ldloc.1
		ldfld int32 MiscPoint::y
		add
		ret
	}

	.method public static int32 test_3_dup_i8 () cil managed
	{
		.maxstack 4
		ldc.i8 3
		dup
		add
		ldc.i8 2
		div
		conv.i4
		ret
	}

	// initobj over a reference-typed local writes null into it.
	.method public static int32 test_1_initobj_reference () cil managed
	{
		.maxstack 2
		.locals init (object held)
		ldstr "not null"
		stloc.0
		ldloca.s 0
		initobj [mscorlib]System.Object
		ldloc.0
		ldnull
		ceq
		ret
	}

	.method public static int32 test_4_sizeof_int32 () cil managed
	{
		.maxstack 1
		sizeof [mscorlib]System.Int32
		ret
	}

	.method public static int32 test_8_sizeof_struct () cil managed
	{
		.maxstack 1
		sizeof MiscPoint
		ret
	}

	// ldtoken of a field, turned back into the field it names.
	.method public static int32 test_1_ldtoken_field () cil managed
	{
		.maxstack 2
		ldtoken field int32 MiscIL::counter
		call class [mscorlib]System.Reflection.FieldInfo [mscorlib]System.Reflection.FieldInfo::GetFieldFromHandle (valuetype [mscorlib]System.RuntimeFieldHandle)
		callvirt instance string [mscorlib]System.Reflection.MemberInfo::get_Name ()
		ldstr "counter"
		call bool [mscorlib]System.String::op_Equality (string, string)
		ret
	}

	// A switch whose table is empty falls straight through.
	.method public static int32 test_5_empty_switch () cil managed
	{
		.maxstack 2
		ldc.i4.0
		switch ()
		ldc.i4.5
		ret
	}

	// A switch reached with a negative index takes the default arm.
	.method public static int32 test_5_negative_switch_index () cil managed
	{
		.maxstack 2
		ldc.i4.m1
		switch (IL_zero, IL_one)
		ldc.i4.5
		ret
	IL_zero:
		ldc.i4.0
		ret
	IL_one:
		ldc.i4.1
		ret
	}

	// readonly. on ldelema drops the type check the store would otherwise get.
	.method public static int32 test_9_readonly_ldelema () cil managed
	{
		.maxstack 4
		.locals init (valuetype MiscPoint[] a)
		ldc.i4.2
		newarr MiscPoint
		stloc.0
		ldloc.0
		ldc.i4.1
		readonly.
		ldelema MiscPoint
		ldc.i4.s 9
		stfld int32 MiscPoint::x
		ldloc.0
		ldc.i4.1
		ldelema MiscPoint
		ldfld int32 MiscPoint::x
		ret
	}

	// An exception filter that selects its handler, written out rather than
	// through C#'s `when`.
	.method public static int32 test_3_endfilter_selects () cil managed
	{
		.maxstack 2
		.locals init (int32 result)
		.try {
			newobj instance void [mscorlib]System.InvalidOperationException::.ctor ()
			throw
		} filter {
			pop
			ldc.i4.1
			endfilter
		} {
			pop
			ldc.i4.3
			stloc.0
			leave IL_done
		}
	IL_done:
		ldloc.0
		ret
	}

	// A filter that declines leaves the exception to an outer handler.
	.method public static int32 test_4_endfilter_declines () cil managed
	{
		.maxstack 2
		.locals init (int32 result)
		.try {
			.try {
				newobj instance void [mscorlib]System.InvalidOperationException::.ctor ()
				throw
			} filter {
				pop
				ldc.i4.0
				endfilter
			} {
				pop
				ldc.i4.1
				stloc.0
				leave IL_done
			}
		} catch [mscorlib]System.InvalidOperationException {
			pop
			ldc.i4.4
			stloc.0
			leave IL_done
		}
	IL_done:
		ldloc.0
		ret
	}
}
