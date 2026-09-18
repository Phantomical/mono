// Array element addresses that need a check C# never asks for, and the
// multi-dimensional Get/Set/Address methods spelled out.

.assembly extern mscorlib { }

.class public auto ansi ArraysIL extends [mscorlib]System.Object
{
	// ldelema on a covariant array. The element type the instruction names is
	// not the array's, so the address can only be handed out after a check.
	.method public static int32 test_1_ldelema_type_check_throws () cil managed
	{
		.maxstack 3
		.locals init (object[] a)
		.try {
			ldc.i4.2
			newarr [mscorlib]System.String
			stloc.0
			ldloc.0
			ldc.i4.0
			ldelema [mscorlib]System.Object
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.ArrayTypeMismatchException {
			pop
			ldc.i4.1
			ret
		}
	}

	// readonly. in front of it drops the check, so the address comes back.
	.method public static int32 test_2_readonly_ldelema_skips_the_check () cil managed
	{
		.maxstack 3
		.locals init (object[] a)
		ldc.i4.2
		newarr [mscorlib]System.String
		stloc.0
		ldloc.0
		ldc.i4.0
		readonly.
		ldelema [mscorlib]System.Object
		ldind.ref
		brfalse IL_null
		ldc.i4.0
		ret
	IL_null:
		ldc.i4.2
		ret
	}

	// The Address method of a rank-2 array, which ldelema turns into there.
	.method public static int32 test_6_rank_two_address () cil managed
	{
		.maxstack 4
		.locals init (int32[,] a)
		ldc.i4.2
		ldc.i4.3
		newobj instance void int32[,]::.ctor (int32, int32)
		stloc.0
		ldloc.0
		ldc.i4.1
		ldc.i4.2
		call instance int32& int32[,]::Address (int32, int32)
		ldc.i4.6
		stind.i4
		ldloc.0
		ldc.i4.1
		ldc.i4.2
		call instance int32 int32[,]::Get (int32, int32)
		ret
	}

	.method public static int32 test_8_rank_two_set_and_get () cil managed
	{
		.maxstack 5
		.locals init (int32[,] a)
		ldc.i4.2
		ldc.i4.2
		newobj instance void int32[,]::.ctor (int32, int32)
		stloc.0
		ldloc.0
		ldc.i4.0
		ldc.i4.1
		ldc.i4.8
		call instance void int32[,]::Set (int32, int32, int32)
		ldloc.0
		ldc.i4.0
		ldc.i4.1
		call instance int32 int32[,]::Get (int32, int32)
		ret
	}

	.method public static int32 test_1_rank_two_get_out_of_range () cil managed
	{
		.maxstack 4
		.locals init (int32[,] a)
		.try {
			ldc.i4.2
			ldc.i4.2
			newobj instance void int32[,]::.ctor (int32, int32)
			stloc.0
			ldloc.0
			ldc.i4.0
			ldc.i4.2
			call instance int32 int32[,]::Get (int32, int32)
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.IndexOutOfRangeException {
			pop
			ldc.i4.1
			ret
		}
	}

	// A rank-3 array, so the index calculation runs over more than two bounds.
	.method public static int32 test_7_rank_three_set_and_get () cil managed
	{
		.maxstack 6
		.locals init (int32[,,] a)
		ldc.i4.2
		ldc.i4.2
		ldc.i4.2
		newobj instance void int32[,,]::.ctor (int32, int32, int32)
		stloc.0
		ldloc.0
		ldc.i4.1
		ldc.i4.0
		ldc.i4.1
		ldc.i4.7
		call instance void int32[,,]::Set (int32, int32, int32, int32)
		ldloc.0
		ldc.i4.1
		ldc.i4.0
		ldc.i4.1
		call instance int32 int32[,,]::Get (int32, int32, int32)
		ret
	}

	// The multi-dimensional constructor that takes a lower bound and a length per
	// dimension, rather than a length each.
	.method public static int32 test_4_rank_two_with_lower_bounds () cil managed
	{
		.maxstack 6
		.locals init (int32[,] a)
		ldc.i4.s 10
		ldc.i4.2
		ldc.i4.s 20
		ldc.i4.2
		newobj instance void int32[,]::.ctor (int32, int32, int32, int32)
		stloc.0
		ldloc.0
		ldc.i4.s 11
		ldc.i4.s 21
		ldc.i4.4
		call instance void int32[,]::Set (int32, int32, int32)
		ldloc.0
		ldc.i4.s 11
		ldc.i4.s 21
		call instance int32 int32[,]::Get (int32, int32)
		ret
	}

	.method public static int32 test_10_lower_bound_is_reported () cil managed
	{
		.maxstack 6
		.locals init (int32[,] a)
		ldc.i4.s 10
		ldc.i4.2
		ldc.i4.s 20
		ldc.i4.2
		newobj instance void int32[,]::.ctor (int32, int32, int32, int32)
		stloc.0
		ldloc.0
		ldc.i4.0
		callvirt instance int32 [mscorlib]System.Array::GetLowerBound (int32)
		ret
	}

	// stelem with a type token rather than one of the width-specific forms.
	.method public static int32 test_3_stelem_with_a_type_token () cil managed
	{
		.maxstack 4
		.locals init (int32[] a)
		ldc.i4.2
		newarr [mscorlib]System.Int32
		stloc.0
		ldloc.0
		ldc.i4.1
		ldc.i4.3
		stelem [mscorlib]System.Int32
		ldloc.0
		ldc.i4.1
		ldelem [mscorlib]System.Int32
		ret
	}

	// A sealed element class is exact, so the store tests the value against it
	// as a constant instead of reading it off the array. C# never writes a
	// site of this shape, because it casts the value first and the cast is
	// what the store then trusts.
	//
	// The value here really is a string, so the inline test answers and the
	// runtime is never asked.
	.method public static int32 test_1_stelem_ref_sealed_element_takes_a_hit () cil managed
	{
		.maxstack 3
		.locals init (string[] a, object v)
		ldstr "x"
		stloc.1
		ldc.i4.2
		newarr [mscorlib]System.String
		stloc.0
		ldloc.0
		ldc.i4.0
		ldloc.1
		stelem.ref
		ldloc.0
		ldc.i4.0
		ldelem.ref
		brfalse IL_empty
		ldc.i4.1
		ret
	IL_empty:
		ldc.i4.0
		ret
	}

	// The same shape with a value of another class. The inline test says no,
	// and the runtime is what draws the exception.
	.method public static int32 test_1_stelem_ref_sealed_element_refuses () cil managed
	{
		.maxstack 3
		.locals init (string[] a)
		.try {
			ldc.i4.2
			newarr [mscorlib]System.String
			stloc.0
			ldloc.0
			ldc.i4.0
			newobj instance void [mscorlib]System.Object::.ctor ()
			stelem.ref
			ldc.i4.0
			ret
		} catch [mscorlib]System.ArrayTypeMismatchException {
			pop
			ldc.i4.1
			ret
		}
	}

	.method public static int32 test_5_ldelem_of_a_reference_type_token () cil managed
	{
		.maxstack 4
		.locals init (string[] a)
		ldc.i4.2
		newarr [mscorlib]System.String
		stloc.0
		ldloc.0
		ldc.i4.0
		ldstr "hello"
		stelem [mscorlib]System.String
		ldloc.0
		ldc.i4.0
		ldelem [mscorlib]System.String
		callvirt instance int32 [mscorlib]System.String::get_Length ()
		ret
	}
}
