// Call shapes a C# compiler does not emit: jmp, an explicit tail. prefix, a
// calli through a function pointer, and a vararg call with its arglist.

.assembly extern mscorlib { }
.assembly 'calls-il' { }

.class public auto ansi CallsIL extends [mscorlib]System.Object
{
	.method public static int32 Target (int32 a, int32 b) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		add
		ret
	}

	// jmp hands the current frame's arguments to another method and does not
	// come back.
	.method public static int32 Jumper (int32 a, int32 b) cil managed
	{
		.maxstack 2
		jmp int32 CallsIL::Target (int32, int32)
	}

	.method public static int32 test_9_jmp () cil managed
	{
		.maxstack 2
		ldc.i4.4
		ldc.i4.5
		call int32 CallsIL::Jumper (int32, int32)
		ret
	}

	// An explicit tail. call: the callee gets the caller's frame.
	.method public static int32 TailCaller (int32 a, int32 b) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		tail. call int32 CallsIL::Target (int32, int32)
		ret
	}

	.method public static int32 test_11_tail_call () cil managed
	{
		.maxstack 2
		ldc.i4.5
		ldc.i4.6
		call int32 CallsIL::TailCaller (int32, int32)
		ret
	}

	// Deep enough that a tail call that made a frame would be visible as stack
	// growth rather than as an answer.
	.method public static int32 TailCount (int32 n, int32 acc) cil managed
	{
		.maxstack 3
		ldarg.0
		brfalse IL_done
		ldarg.0
		ldc.i4.1
		sub
		ldarg.1
		ldc.i4.1
		add
		tail. call int32 CallsIL::TailCount (int32, int32)
		ret
	IL_done:
		ldarg.1
		ret
	}

	.method public static int32 test_1000_tail_recursion () cil managed
	{
		.maxstack 2
		ldc.i4 1000
		ldc.i4.0
		call int32 CallsIL::TailCount (int32, int32)
		ret
	}

	// calli through a function pointer taken with ldftn.
	.method public static int32 test_7_calli () cil managed
	{
		.maxstack 3
		ldc.i4.3
		ldc.i4.4
		ldftn int32 CallsIL::Target (int32, int32)
		calli int32 (int32, int32)
		ret
	}

	// A vararg method reads its variable arguments through an ArgIterator built
	// from arglist.
	.method public static vararg int32 SumVarargs (int32 count) cil managed
	{
		.maxstack 4
		.locals init (valuetype [mscorlib]System.ArgIterator it,
		              int32 total, int32 i)
		ldloca.s 0
		arglist
		call instance void [mscorlib]System.ArgIterator::.ctor (valuetype [mscorlib]System.RuntimeArgumentHandle)
		ldc.i4.0
		stloc.1
		ldc.i4.0
		stloc.2
	IL_loop:
		ldloc.2
		ldarg.0
		bge IL_done
		ldloc.1
		ldloca.s 0
		call instance typedref [mscorlib]System.ArgIterator::GetNextArg ()
		refanyval [mscorlib]System.Int32
		ldind.i4
		add
		stloc.1
		ldloc.2
		ldc.i4.1
		add
		stloc.2
		br IL_loop
	IL_done:
		ldloc.1
		ret
	}

	.method public static int32 test_6_vararg_call () cil managed
	{
		.maxstack 4
		ldc.i4.3
		ldc.i4.1
		ldc.i4.2
		ldc.i4.3
		call vararg int32 CallsIL::SumVarargs (int32, ..., int32, int32, int32)
		ret
	}
}
