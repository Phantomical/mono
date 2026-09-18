// Call shapes a C# compiler does not emit: jmp, an explicit tail. prefix, a
// calli through a function pointer, and a vararg call with its arglist.

.assembly extern mscorlib { }
.module extern 'interp-tests-cs.netmodule'

.class public auto ansi CallsIL extends [mscorlib]System.Object
{
	.custom instance void [.module 'interp-tests-cs.netmodule']NoOptAttribute::.ctor() = ( 01 00 00 00 )
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

	// Deep enough that the tail call has to reuse the frame rather than make a
	// new one.
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

	// A vararg call whose variable part is not all one type, so the cookie the
	// caller builds has to describe each argument separately.
	.method public static vararg int32 MixedVarargs (int32 count) cil managed
	{
		.maxstack 4
		.locals init (valuetype [mscorlib]System.ArgIterator it,
		              int32 total, class [mscorlib]System.TypedReference dummy)
		ldloca.s 0
		arglist
		call instance void [mscorlib]System.ArgIterator::.ctor (valuetype [mscorlib]System.RuntimeArgumentHandle)
		ldloca.s 0
		call instance typedref [mscorlib]System.ArgIterator::GetNextArg ()
		refanyval [mscorlib]System.Int32
		ldind.i4
		stloc.1
		ldloc.1
		ldloca.s 0
		call instance typedref [mscorlib]System.ArgIterator::GetNextArg ()
		refanyval [mscorlib]System.Double
		ldind.r8
		conv.i4
		add
		stloc.1
		ldloc.1
		ldloca.s 0
		call instance typedref [mscorlib]System.ArgIterator::GetNextArg ()
		refanyval [mscorlib]System.String
		ldind.ref
		callvirt instance int32 [mscorlib]System.String::get_Length ()
		add
		ret
	}

	.method public static int32 test_9_vararg_mixed_types () cil managed
	{
		.maxstack 5
		ldc.i4.3
		ldc.i4.2
		ldc.r8 3.5
		ldstr "abcd"
		call vararg int32 CallsIL::MixedVarargs (int32, ..., int32, float64, string)
		ret
	}

	// A vararg method calling itself.  Each frame holds a cookie of its own for
	// as long as it runs, so the allocator those come out of has to grow past
	// its first fragment.
	.method public static vararg int32 DeepVarargs (int32 depth) cil managed
	{
		.maxstack 5
		.locals init (valuetype [mscorlib]System.ArgIterator it, int32 taken)
		ldloca.s 0
		arglist
		call instance void [mscorlib]System.ArgIterator::.ctor (valuetype [mscorlib]System.RuntimeArgumentHandle)
		ldloca.s 0
		call instance typedref [mscorlib]System.ArgIterator::GetNextArg ()
		refanyval [mscorlib]System.Int32
		ldind.i4
		stloc.1
		ldarg.0
		brfalse IL_done
		ldarg.0
		ldc.i4.1
		sub
		ldloc.1
		call vararg int32 CallsIL::DeepVarargs (int32, ..., int32)
		ldloc.1
		add
		ret
	IL_done:
		ldloc.1
		ret
	}

	.method public static int32 test_201_vararg_recursion () cil managed
	{
		.maxstack 4
		ldc.i4 200
		ldc.i4.1
		call vararg int32 CallsIL::DeepVarargs (int32, ..., int32)
		ret
	}

	// A vararg call with no variable arguments at all: arglist still has to
	// produce an iterator that reports none.
	.method public static vararg int32 EmptyVarargs (int32 seed) cil managed
	{
		.maxstack 3
		.locals init (valuetype [mscorlib]System.ArgIterator it)
		ldloca.s 0
		arglist
		call instance void [mscorlib]System.ArgIterator::.ctor (valuetype [mscorlib]System.RuntimeArgumentHandle)
		ldarg.0
		ldloca.s 0
		call instance int32 [mscorlib]System.ArgIterator::GetRemainingCount ()
		add
		ret
	}

	.method public static int32 test_4_vararg_with_no_variable_part () cil managed
	{
		.maxstack 2
		ldc.i4.4
		call vararg int32 CallsIL::EmptyVarargs (int32)
		ret
	}

	// calli with an unmanaged signature, which is a different opcode from the
	// managed one and takes the interpreter through a native wrapper it builds
	// on the spot.
	.method public static pinvokeimpl("__Internal" as "interp_test_symbol" cdecl)
	        native int symbol (string name) cil managed preservesig { }

	.method public static int32 test_7_calli_native () cil managed
	{
		.maxstack 2
		ldc.i4.s -7
		ldstr "abs"
		call native int CallsIL::symbol (string)
		calli unmanaged cdecl int32 (int32)
		ret
	}
}
