// CIL shapes that C# never writes, aimed at the arms of the interpreter
// transform no other corpus reaches: the long forms of the argument and local
// opcodes, cpobj over a reference type, unbox over Nullable, ldfld and stfld
// naming a static field, and newobj on an abstract class.
//
// A method named test_<n>_<what> is a test, and it passes when it returns <n>.
//
// A callee under 20 bytes of IL is inlined into its caller, and an inlined body
// takes a different arm of every one of the argument and local opcodes. So each
// of those opcodes gets two tests: one through a helper marked noinlining, and
// one through a helper small enough to inline.

.assembly extern mscorlib { }
.assembly 'generate-il' { }

.class public sequential ansi GenerateILPair extends [mscorlib]System.ValueType
{
	.field public int32 first
	.field public int32 second
}

.class public auto ansi sealed GenerateILColour extends [mscorlib]System.Enum
{
	.field public specialname rtspecialname int32 value__
	.field public static literal valuetype GenerateILColour Red = int32(0)
	.field public static literal valuetype GenerateILColour Blue = int32(2)
}

.class public abstract auto ansi GenerateILAbstract extends [mscorlib]System.Object
{
	.method public specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ret
	}
}

// Bodies small enough for the inliner. Every one of them uses a long form.
.class public auto ansi GenerateILInline extends [mscorlib]System.Object
{
	.method public static int32 IdLong (int32 x) cil managed
	{
		.maxstack 1
		ldarg 0
		ret
	}

	.method public static int32 BumpLong (int32 x) cil managed
	{
		.maxstack 2
		ldarg 0
		ldc.i4.3
		add
		starg 0
		ldarg 0
		ret
	}

	.method public static int32 AddrLong (int32 x) cil managed
	{
		.maxstack 1
		ldarga 0
		ldind.i4
		ret
	}

	.method public static int32 LocalAddrLong (int32 x) cil managed
	{
		.maxstack 1
		.locals init (int32 held)
		ldarg 0
		stloc.0
		ldloca 0
		ldind.i4
		ret
	}
}

.class public auto ansi GenerateIL extends [mscorlib]System.Object
{
	.field public static int32 counter
	.field public static object ref_slot
	.field public static valuetype GenerateILPair pair_slot

	// --- helpers the inliner must leave alone ---

	.method public static int32 AddLong (int32 a, int32 b) cil managed noinlining
	{
		.maxstack 2
		ldarg 0
		ldarg 1
		add
		ret
	}

	.method public static int32 PickSixth (int32 a, int32 b, int32 c,
	                                       int32 d, int32 e, int32 f) cil managed noinlining
	{
		.maxstack 1
		ldarg 5
		ret
	}

	.method public static int64 AddLongI8 (int64 a, int64 b) cil managed noinlining
	{
		.maxstack 2
		ldarg 0
		ldarg 1
		add
		ret
	}

	.method public static float64 AddLongR8 (float64 a, float64 b) cil managed noinlining
	{
		.maxstack 2
		ldarg 0
		ldarg 1
		add
		ret
	}

	.method public static int32 ReadArgAddr (int32 a, int32 b) cil managed noinlining
	{
		.maxstack 1
		ldarga 1
		ldind.i4
		ret
	}

	.method public static int32 WriteArgAddr (int32 a) cil managed noinlining
	{
		.maxstack 2
		ldarga 0
		ldc.i4 20
		stind.i4
		ldarg 0
		ret
	}

	.method public static int32 PairFirstAddr (valuetype GenerateILPair p) cil managed noinlining
	{
		.maxstack 1
		ldarga 0
		ldfld int32 GenerateILPair::first
		ret
	}

	.method public static int32 BumpArg (int32 a) cil managed noinlining
	{
		.maxstack 2
		ldarg 0
		ldc.i4 4
		add
		starg 0
		ldarg 0
		ret
	}

	.method public static float64 BumpArgR8 (float64 a) cil managed noinlining
	{
		.maxstack 2
		ldarg 0
		ldc.r8 0.5
		add
		starg 0
		ldarg 0
		ret
	}

	// ldloca long form where the locals sit behind two arguments, so the
	// index the transform emits is not the one in the instruction.
	.method public static int32 LocalAddr (int32 a, int32 b) cil managed noinlining
	{
		.maxstack 2
		.locals init (int32 first, int32 held)
		ldarg 0
		ldarg 1
		add
		stloc.1
		ldloca 1
		ldind.i4
		ret
	}

	.method public static object MakeAbstract () cil managed noinlining
	{
		.maxstack 1
		newobj instance void GenerateILAbstract::.ctor ()
		ret
	}

	// --- ldarg, long form ---

	.method public static int32 test_7_ldarg_long_form () cil managed
	{
		.maxstack 2
		ldc.i4 3
		ldc.i4 4
		call int32 GenerateIL::AddLong (int32, int32)
		ret
	}

	.method public static int32 test_6_ldarg_long_form_sixth_arg () cil managed
	{
		.maxstack 6
		ldc.i4.1
		ldc.i4.2
		ldc.i4.3
		ldc.i4.4
		ldc.i4.5
		ldc.i4.6
		call int32 GenerateIL::PickSixth (int32, int32, int32, int32, int32, int32)
		ret
	}

	.method public static int32 test_1_ldarg_long_form_i8 () cil managed
	{
		.maxstack 4
		ldc.i8 4000000000
		ldc.i8 1
		call int64 GenerateIL::AddLongI8 (int64, int64)
		ldc.i8 4000000001
		ceq
		ret
	}

	.method public static int32 test_1_ldarg_long_form_r8 () cil managed
	{
		.maxstack 4
		ldc.r8 1.5
		ldc.r8 2.25
		call float64 GenerateIL::AddLongR8 (float64, float64)
		ldc.r8 3.75
		ceq
		ret
	}

	.method public static int32 test_5_ldarg_long_form_inlined () cil managed
	{
		.maxstack 2
		ldc.i4 5
		call int32 GenerateILInline::IdLong (int32)
		ret
	}

	// --- ldarga, long form ---

	.method public static int32 test_9_ldarga_long_form () cil managed
	{
		.maxstack 2
		ldc.i4 4
		ldc.i4 9
		call int32 GenerateIL::ReadArgAddr (int32, int32)
		ret
	}

	.method public static int32 test_20_ldarga_long_form_store () cil managed
	{
		.maxstack 2
		ldc.i4 3
		call int32 GenerateIL::WriteArgAddr (int32)
		ret
	}

	.method public static int32 test_5_ldarga_long_form_valuetype () cil managed
	{
		.maxstack 3
		.locals init (valuetype GenerateILPair p)
		ldloca.s 0
		initobj GenerateILPair
		ldloca.s 0
		ldc.i4 5
		stfld int32 GenerateILPair::first
		ldloc.0
		call int32 GenerateIL::PairFirstAddr (valuetype GenerateILPair)
		ret
	}

	// Local 0 holds a value the callee must not see: the inlined arm of
	// CEE_LDARGA takes the address of local n rather than of the argument.
	.method public static int32 test_5_ldarga_long_form_inlined () cil managed
	{
		.maxstack 2
		.locals init (int32 witness)
		ldc.i4 99
		stloc.0
		ldc.i4 5
		call int32 GenerateILInline::AddrLong (int32)
		ret
	}

	// --- starg, long form ---

	.method public static int32 test_10_starg_long_form () cil managed
	{
		.maxstack 2
		ldc.i4 6
		call int32 GenerateIL::BumpArg (int32)
		ret
	}

	.method public static int32 test_1_starg_long_form_r8 () cil managed
	{
		.maxstack 4
		ldc.r8 2.0
		call float64 GenerateIL::BumpArgR8 (float64)
		ldc.r8 2.5
		ceq
		ret
	}

	.method public static int32 test_8_starg_long_form_inlined () cil managed
	{
		.maxstack 2
		ldc.i4 5
		call int32 GenerateILInline::BumpLong (int32)
		ret
	}

	// --- ldloca, long form ---

	.method public static int32 test_11_ldloca_long_form () cil managed
	{
		.maxstack 2
		.locals init (int32 held)
		ldc.i4 11
		stloc.0
		ldloca 0
		ldind.i4
		ret
	}

	.method public static int32 test_3_ldloca_long_form_store () cil managed
	{
		.maxstack 2
		.locals init (int32 held)
		ldloca 0
		ldc.i4 3
		stind.i4
		ldloc.0
		ret
	}

	.method public static int32 test_4_ldloca_long_form_valuetype () cil managed
	{
		.maxstack 2
		.locals init (valuetype GenerateILPair p)
		ldloca 0
		initobj GenerateILPair
		ldloca 0
		ldc.i4 4
		stfld int32 GenerateILPair::first
		ldloca 0
		ldfld int32 GenerateILPair::first
		ret
	}

	.method public static int32 test_8_ldloca_long_form_behind_args () cil managed
	{
		.maxstack 2
		ldc.i4 3
		ldc.i4 5
		call int32 GenerateIL::LocalAddr (int32, int32)
		ret
	}

	.method public static int32 test_6_ldloca_long_form_inlined () cil managed
	{
		.maxstack 2
		.locals init (int32 witness)
		ldc.i4 98
		stloc.0
		ldc.i4 6
		call int32 GenerateILInline::LocalAddrLong (int32)
		ret
	}

	// --- cpobj over a reference type ---

	.method public static int32 test_1_cpobj_object () cil managed
	{
		.maxstack 2
		.locals init (object src, object dst)
		newobj instance void [mscorlib]System.Object::.ctor ()
		stloc.0
		ldloca.s 1
		ldloca.s 0
		cpobj [mscorlib]System.Object
		ldloc.1
		ldloc.0
		ceq
		ret
	}

	.method public static int32 test_1_cpobj_string () cil managed
	{
		.maxstack 2
		.locals init (string src, string dst)
		ldstr "generate-il"
		stloc.0
		ldloca.s 1
		ldloca.s 0
		cpobj [mscorlib]System.String
		ldloc.1
		ldstr "generate-il"
		ceq
		ret
	}

	// The destination is a static slot rather than a local, so the address
	// comes from ldsflda.
	.method public static int32 test_1_cpobj_object_into_static () cil managed
	{
		.maxstack 2
		.locals init (object src)
		newobj instance void [mscorlib]System.Object::.ctor ()
		stloc.0
		ldsflda object GenerateIL::ref_slot
		ldloca.s 0
		cpobj [mscorlib]System.Object
		ldsfld object GenerateIL::ref_slot
		ldloc.0
		ceq
		ret
	}

	.method public static int32 test_1_cpobj_object_null_source () cil managed
	{
		.maxstack 2
		.locals init (object src, object dst)
		newobj instance void [mscorlib]System.Object::.ctor ()
		stloc.1
		ldloca.s 1
		ldloca.s 0
		cpobj [mscorlib]System.Object
		ldloc.1
		ldnull
		ceq
		ret
	}

	// --- unbox over Nullable ---

	.method public static int32 test_42_unbox_nullable () cil managed
	{
		.maxstack 2
		ldc.i4 42
		box [mscorlib]System.Int32
		unbox valuetype [mscorlib]System.Nullable`1<int32>
		call instance !0 valuetype [mscorlib]System.Nullable`1<int32>::get_Value ()
		ret
	}

	// Nullable.Unbox answers an empty value rather than throwing, which is
	// what separates unbox over Nullable from unbox over any other type.
	.method public static int32 test_1_unbox_nullable_null () cil managed
	{
		.maxstack 2
		ldnull
		unbox valuetype [mscorlib]System.Nullable`1<int32>
		call instance bool valuetype [mscorlib]System.Nullable`1<int32>::get_HasValue ()
		ldc.i4.0
		ceq
		ret
	}

	.method public static int32 test_1_unbox_nullable_i8 () cil managed
	{
		.maxstack 4
		ldc.i8 4000000001
		box [mscorlib]System.Int64
		unbox valuetype [mscorlib]System.Nullable`1<int64>
		call instance !0 valuetype [mscorlib]System.Nullable`1<int64>::get_Value ()
		ldc.i8 4000000001
		ceq
		ret
	}

	// An enum parameter picks Nullable.UnboxExact, which is the arm that
	// checks the boxed type before it converts.
	.method public static int32 test_2_unbox_nullable_enum () cil managed
	{
		.maxstack 2
		ldc.i4.2
		box GenerateILColour
		unbox valuetype [mscorlib]System.Nullable`1<valuetype GenerateILColour>
		call instance !0 valuetype [mscorlib]System.Nullable`1<valuetype GenerateILColour>::get_Value ()
		ret
	}

	.method public static int32 test_1_unbox_nullable_enum_wrong_type () cil managed
	{
		.maxstack 2
		.try {
			ldc.i4.2
			box [mscorlib]System.Int32
			unbox valuetype [mscorlib]System.Nullable`1<valuetype GenerateILColour>
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.InvalidCastException {
			pop
			ldc.i4.1
			ret
		}
	}

	.method public static int32 test_1_unbox_nullable_enum_null () cil managed
	{
		.maxstack 2
		ldnull
		unbox valuetype [mscorlib]System.Nullable`1<valuetype GenerateILColour>
		call instance bool valuetype [mscorlib]System.Nullable`1<valuetype GenerateILColour>::get_HasValue ()
		ldc.i4.0
		ceq
		ret
	}

	// --- ldfld and stfld naming a static field ---

	.method public static int32 test_7_ldfld_static_field () cil managed
	{
		.maxstack 2
		ldc.i4 7
		stsfld int32 GenerateIL::counter
		newobj instance void [mscorlib]System.Object::.ctor ()
		ldfld int32 GenerateIL::counter
		ret
	}

	// The object the instruction pops is unused, so a null one is enough.
	.method public static int32 test_13_ldfld_static_field_null_object () cil managed
	{
		.maxstack 2
		ldc.i4 13
		stsfld int32 GenerateIL::counter
		ldnull
		ldfld int32 GenerateIL::counter
		ret
	}

	.method public static int32 test_1_ldfld_static_ref_field () cil managed
	{
		.maxstack 2
		newobj instance void [mscorlib]System.Object::.ctor ()
		stsfld object GenerateIL::ref_slot
		ldnull
		ldfld object GenerateIL::ref_slot
		ldsfld object GenerateIL::ref_slot
		ceq
		ret
	}

	.method public static int32 test_9_stfld_static_field () cil managed
	{
		.maxstack 2
		ldnull
		ldc.i4 9
		stfld int32 GenerateIL::counter
		ldsfld int32 GenerateIL::counter
		ret
	}

	.method public static int32 test_15_stfld_static_field_real_object () cil managed
	{
		.maxstack 2
		newobj instance void [mscorlib]System.Object::.ctor ()
		ldc.i4 15
		stfld int32 GenerateIL::counter
		ldsfld int32 GenerateIL::counter
		ret
	}

	.method public static int32 test_3_stfld_static_valuetype_field () cil managed
	{
		.maxstack 3
		.locals init (valuetype GenerateILPair p)
		ldloca.s 0
		initobj GenerateILPair
		ldloca.s 0
		ldc.i4 3
		stfld int32 GenerateILPair::first
		ldnull
		ldloc.0
		stfld valuetype GenerateILPair GenerateIL::pair_slot
		ldsflda valuetype GenerateILPair GenerateIL::pair_slot
		ldfld int32 GenerateILPair::first
		ret
	}

	.method public static int32 test_6_ldfld_static_valuetype_field () cil managed
	{
		.maxstack 3
		.locals init (valuetype GenerateILPair p)
		ldloca.s 0
		initobj GenerateILPair
		ldloca.s 0
		ldc.i4 6
		stfld int32 GenerateILPair::second
		ldloc.0
		stsfld valuetype GenerateILPair GenerateIL::pair_slot
		ldnull
		ldfld valuetype GenerateILPair GenerateIL::pair_slot
		ldfld int32 GenerateILPair::second
		ret
	}

	// --- newobj on an abstract class ---

	.method public static int32 test_1_newobj_abstract_throws () cil managed
	{
		.maxstack 2
		.try {
			call object GenerateIL::MakeAbstract ()
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.MemberAccessException {
			pop
			ldc.i4.1
			ret
		}
	}
}
