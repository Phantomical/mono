// The sizes the transform refuses.
//
// A method's locals are packed into one region addressed by a 16-bit offset, so
// a body whose locals reach 64 KB cannot be run. `.size` gives a value type a
// size without fields, which is what makes the case small enough to read.

.assembly extern mscorlib { }
.module extern 'interp-tests-cs.netmodule'

.class public sequential ansi sealed LimitsBlock extends [mscorlib]System.ValueType
{
	.size 32768
}

.class public auto ansi Limits extends [mscorlib]System.Object
{
	.custom instance void [.module 'interp-tests-cs.netmodule']InstrumentedAttribute::.ctor() = ( 01 00 00 00 )
	// Three of them, so the locals region passes 64 KB whatever else the
	// transform adds.
	.method public static int32 TooManyLocals () cil managed noinlining
	{
		.maxstack 2
		.locals init (valuetype LimitsBlock a, valuetype LimitsBlock b,
		              valuetype LimitsBlock c)
		ldloca.s 0
		initobj LimitsBlock
		ldloca.s 1
		initobj LimitsBlock
		ldloca.s 2
		initobj LimitsBlock
		ldc.i4.7
		ret
	}

	.method public static int32 test_1_locals_size_too_big () cil managed
	{
		.maxstack 2
		.try {
			call int32 Limits::TooManyLocals ()
			pop
			ldc.i4.0
			ret
		} catch [mscorlib]System.InvalidProgramException {
			pop
			ldc.i4.1
			ret
		}
	}

	// One block is well inside the limit, so the same shape runs.
	.method public static int32 test_5_one_block_of_locals_is_fine () cil managed
	{
		.maxstack 2
		.locals init (valuetype LimitsBlock a)
		ldloca.s 0
		initobj LimitsBlock
		ldc.i4.5
		ret
	}
}
