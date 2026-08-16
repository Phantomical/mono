// IL the interpreter runs and the LLVM translator declines.  Under `check-interp`
// these pass; under `check-interp-jit`, where there is no interpreter to fall
// back to, a refusal is what the test reports.

.assembly extern mscorlib { }

.class public sequential ansi sealed RefusalCounter extends [mscorlib]System.ValueType
{
	.field public int32 held

	.method public virtual instance int32 GetHashCode () cil managed
	{
		.maxstack 2
		ldarg.0
		ldfld int32 RefusalCounter::held
		ret
	}
}

.class public auto ansi Refusals extends [mscorlib]System.Object
{
	// constrained. in front of a plain call rather than a callvirt.  ECMA-335
	// III.2.1 puts the prefix on callvirt; the interpreter honours it here too.
	.method public static int32 test_5_constrained_on_call () cil managed
	{
		.maxstack 3
		.locals init (valuetype RefusalCounter c)
		ldloca.s 0
		ldc.i4.5
		stfld int32 RefusalCounter::held
		ldloca.s 0
		constrained. RefusalCounter
		call instance int32 [mscorlib]System.Object::GetHashCode ()
		ret
	}

	// localloc reached with something else still on the evaluation stack.
	.method public static int32 test_9_localloc_with_stack () cil managed
	{
		.maxstack 4
		.locals init (native int block)
		ldc.i4.s 9
		ldc.i4 32
		localloc
		stloc.0
		ret
	}

	// An unaligned. prefix with an alignment ECMA-335 III.2.5 does not define.
	.method public static int32 test_7_unaligned_three () cil managed
	{
		.maxstack 3
		.locals init (int32 held)
		ldloca.s 0
		ldc.i4.7
		stind.i4
		ldloca.s 0
		unaligned. 3
		ldind.i4
		ret
	}
}
