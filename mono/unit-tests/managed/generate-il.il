// CIL shapes that C# never writes, aimed at the arms of the interpreter
// transform no other corpus reaches: the long forms of the argument and local
// opcodes, cpobj over a reference type, unbox over Nullable, ldfld and stfld
// naming a static field, and newobj on an abstract class.
//
// A callee under 20 bytes of IL is inlined into its caller. An inlined body
// takes a different arm of every argument and local opcode. So each of those
// opcodes gets two tests: one through a helper marked noinlining, and one
// through a helper small enough to inline.
//
// A helper mixes the long form under test with the short form of its sibling,
// so the answer covers the index each of the two used.

.assembly extern mscorlib { }

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

// Bodies small enough for the inliner. Each one carries one long form.
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
		ldarg.0
		ldc.i4.3
		add
		starg 0
		ldarg.0
		ret
	}

	.method public static int32 AddrLong (int32 x) cil managed
	{
		.maxstack 1
		ldarga 0
		ldind.i4
		ret
	}

	.method public static int32 StoreLong (int32 x) cil managed
	{
		.maxstack 1
		.locals init (int32 held)
		ldarg.0
		stloc 0
		ldloc.0
		ret
	}

	.method public static int32 FetchLong (int32 x) cil managed
	{
		.maxstack 1
		.locals init (int32 held)
		ldarg.0
		stloc.0
		ldloc 0
		ret
	}

	.method public static int32 LocalAddrLong (int32 x) cil managed
	{
		.maxstack 1
		.locals init (int32 held)
		ldarg.0
		stloc.0
		ldloca 0
		ldind.i4
		ret
	}
}

.class public auto ansi GenerateIL extends [mscorlib]System.Object
{
	.field public static int32 counter
	.field public static valuetype GenerateILPair pair_slot

	// --- helpers the inliner must leave alone ---

	// Subtraction rather than addition, so swapped argument indices give a
	// different answer.
	.method public static int32 SubLong (int32 a, int32 b) cil managed noinlining
	{
		.maxstack 2
		ldarg 0
		ldarg 1
		sub
		ret
	}

	.method public static int32 PickSixth (int32 a, int32 b, int32 c,
	                                       int32 d, int32 e, int32 f) cil managed noinlining
	{
		.maxstack 1
		ldarg 5
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
		ldarg.0
		ret
	}

	.method public static int32 BumpArg (int32 a) cil managed noinlining
	{
		.maxstack 2
		ldarg.0
		ldc.i4 4
		add
		starg 0
		ldarg.0
		ret
	}

	// The locals sit behind two arguments, so the index the transform emits is
	// not the one in the instruction. A local that lands on argument 0 changes
	// what the ldarg.0 below reads.
	.method public static int32 LocalBehindArgs (int32 a, int32 b) cil managed noinlining
	{
		.maxstack 2
		.locals init (int32 held)
		ldarg.1
		ldc.i4 4
		add
		stloc 0
		ldarg.0
		ldloc 0
		add
		ret
	}

	.method public static int32 LocalAddr (int32 a, int32 b) cil managed noinlining
	{
		.maxstack 2
		.locals init (int32 first, int32 held)
		ldarg.0
		ldarg.1
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
		ldc.i4 9
		ldc.i4 2
		call int32 GenerateIL::SubLong (int32, int32)
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

	// Local 0 holds a value the callee must not see.
	.method public static int32 test_5_ldarg_long_form_inlined () cil managed
	{
		.maxstack 2
		.locals init (int32 witness)
		ldc.i4 97
		stloc.0
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

	// Local 0 holds a value the callee must not see.
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

	.method public static int32 test_8_starg_long_form_inlined () cil managed
	{
		.maxstack 2
		.locals init (int32 witness)
		ldc.i4 96
		stloc.0
		ldc.i4 5
		call int32 GenerateILInline::BumpLong (int32)
		ret
	}

	// --- ldloc and stloc, long form ---

	.method public static int32 test_12_ldloc_stloc_behind_args () cil managed
	{
		.maxstack 2
		ldc.i4 3
		ldc.i4 5
		call int32 GenerateIL::LocalBehindArgs (int32, int32)
		ret
	}

	// The callee writes through the long form and reads back through the short
	// one, so the two agree only where the long form names the callee's local.
	.method public static int32 test_7_stloc_long_form_inlined () cil managed
	{
		.maxstack 2
		.locals init (int32 witness)
		ldc.i4 95
		stloc.0
		ldc.i4 7
		call int32 GenerateILInline::StoreLong (int32)
		ret
	}

	// Local 0 holds a value the callee must not see.
	.method public static int32 test_4_ldloc_long_form_inlined () cil managed
	{
		.maxstack 2
		.locals init (int32 witness)
		ldc.i4 94
		stloc.0
		ldc.i4 4
		call int32 GenerateILInline::FetchLong (int32)
		ret
	}

	// --- ldloca, long form ---

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

	.method public static int32 test_8_ldloca_long_form_behind_args () cil managed
	{
		.maxstack 2
		ldc.i4 3
		ldc.i4 5
		call int32 GenerateIL::LocalAddr (int32, int32)
		ret
	}

	// Local 0 holds a value the callee must not see.
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

	// The destination starts out non-null, so a copy that never happens fails
	// here and passes above.
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

	// Nullable.Unbox gives an empty value instead of an exception, which
	// separates unbox over Nullable from unbox over any other type.
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

	// An enum parameter picks Nullable.UnboxExact, which is the arm that checks
	// the boxed type before it converts.
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

	// --- ldfld and stfld naming a static field ---

	// The object the instruction pops is unused, so a null one is enough.
	.method public static int32 test_13_ldfld_static_field () cil managed
	{
		.maxstack 2
		ldc.i4 13
		stsfld int32 GenerateIL::counter
		ldnull
		ldfld int32 GenerateIL::counter
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

	.method public static int32 test_9_stfld_static_field () cil managed
	{
		.maxstack 2
		ldnull
		ldc.i4 9
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
