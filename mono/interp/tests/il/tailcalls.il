// tail. sites, one per shape interp_tail_call_refusal decides on.
//
// A declined tail call is an ordinary call, so nothing a program can see tells
// the two apart. What these check is that the answer is right either way.

.assembly extern mscorlib { }
.module extern 'interp-tests-cs.netmodule'

.class public auto ansi TailCalls extends [mscorlib]System.Object
{
	.custom instance void [.module 'interp-tests-cs.netmodule']InstrumentedAttribute::.ctor() = ( 01 00 00 00 )
	.field public static int32 depth

	.method public static int32 Add (int32 a, int32 b) cil managed
	{
		.maxstack 2
		ldarg.0
		ldarg.1
		add
		ret
	}

	// Self tail recursion, which the transform turns into a branch to the top
	// rather than a call.
	.method public static int32 Countdown (int32 n, int32 acc) cil managed
	{
		.maxstack 3
		ldarg.0
		brfalse IL_done
		ldarg.0
		ldc.i4.1
		sub
		ldarg.1
		ldarg.0
		add
		tail. call int32 TailCalls::Countdown (int32, int32)
		ret
	IL_done:
		ldarg.1
		ret
	}

	.method public static int32 test_55_self_tail_recursion () cil managed
	{
		.maxstack 2
		ldc.i4.s 10
		ldc.i4.0
		call int32 TailCalls::Countdown (int32, int32)
		ret
	}

	// Two methods calling each other in tail position.  Neither frame may grow.
	.method public static int32 PingTail (int32 n) cil managed
	{
		.maxstack 2
		ldarg.0
		brfalse IL_zero
		ldarg.0
		ldc.i4.1
		sub
		tail. call int32 TailCalls::PongTail (int32)
		ret
	IL_zero:
		ldc.i4.3
		ret
	}

	.method public static int32 PongTail (int32 n) cil managed
	{
		.maxstack 2
		ldarg.0
		tail. call int32 TailCalls::PingTail (int32)
		ret
	}

	.method public static int32 test_3_mutual_tail_recursion () cil managed
	{
		.maxstack 2
		ldc.i4 2000
		call int32 TailCalls::PingTail (int32)
		ret
	}

	// A tail site inside a protected block is declined: III.2.4 forbids the
	// prefix there.
	.method public static int32 InTry (int32 a, int32 b) cil managed
	{
		.maxstack 3
		.locals init (int32 result)
		.try {
			ldarg.0
			ldarg.1
			tail. call int32 TailCalls::Add (int32, int32)
			stloc.0
			leave IL_done
		} catch [mscorlib]System.Exception {
			pop
			ldc.i4.m1
			stloc.0
			leave IL_done
		}
	IL_done:
		ldloc.0
		ret
	}

	.method public static int32 test_9_tail_site_in_a_try () cil managed
	{
		.maxstack 2
		ldc.i4.4
		ldc.i4.5
		call int32 TailCalls::InTry (int32, int32)
		ret
	}

	// A tail call to a virtual method: the target is resolved before the frame
	// changes hands.
	.method public static int32 TailVirtual (class TailCallsBase target) cil managed
	{
		.maxstack 2
		ldarg.0
		tail. callvirt instance int32 TailCallsBase::Number ()
		ret
	}

	.method public static int32 test_7_tail_callvirt () cil managed
	{
		.maxstack 2
		newobj instance void TailCallsDerived::.ctor ()
		call int32 TailCalls::TailVirtual (class TailCallsBase)
		ret
	}

	// A tail call to an internal call is declined.
	.method public static int32 test_1_tail_to_internal_call () cil managed
	{
		.maxstack 2
		ldstr "abc"
		tail. callvirt instance int32 [mscorlib]System.Object::GetHashCode ()
		pop
		ldc.i4.1
		ret
	}

	// A tail call whose callee returns a struct.
	.method public static valuetype TailCallsPair MakePair (int32 a) cil managed
	{
		.maxstack 3
		.locals init (valuetype TailCallsPair p)
		ldloca.s 0
		ldarg.0
		stfld int32 TailCallsPair::first
		ldloca.s 0
		ldarg.0
		ldc.i4.1
		add
		stfld int32 TailCallsPair::second
		ldloc.0
		ret
	}

	.method public static valuetype TailCallsPair TailToPair (int32 a) cil managed
	{
		.maxstack 2
		ldarg.0
		tail. call valuetype TailCallsPair TailCalls::MakePair (int32)
		ret
	}

	.method public static int32 test_11_tail_struct_return () cil managed
	{
		.maxstack 2
		.locals init (valuetype TailCallsPair p)
		ldc.i4.5
		call valuetype TailCallsPair TailCalls::TailToPair (int32)
		stloc.0
		ldloc.0
		ldfld int32 TailCallsPair::first
		ldloc.0
		ldfld int32 TailCallsPair::second
		add
		ret
	}

	// A tail call to a method taking more arguments than the caller has, so the
	// argument area has to grow rather than being written over in place.
	.method public static int32 Six (int32 a, int32 b, int32 c, int32 d, int32 e, int32 f) cil managed
	{
		.maxstack 4
		ldarg.0
		ldarg.1
		add
		ldarg.2
		add
		ldarg.3
		add
		ldarg.s 4
		add
		ldarg.s 5
		add
		ret
	}

	.method public static int32 TailToSix (int32 a) cil managed
	{
		.maxstack 8
		ldarg.0
		ldc.i4.1
		ldc.i4.2
		ldc.i4.3
		ldc.i4.4
		ldc.i4.5
		tail. call int32 TailCalls::Six (int32, int32, int32, int32, int32, int32)
		ret
	}

	.method public static int32 test_25_tail_to_more_arguments () cil managed
	{
		.maxstack 2
		ldc.i4.s 10
		call int32 TailCalls::TailToSix (int32)
		ret
	}
}

.class public sequential ansi sealed TailCallsPair extends [mscorlib]System.ValueType
{
	.field public int32 first
	.field public int32 second
}

.class public auto ansi TailCallsBase extends [mscorlib]System.Object
{
	.method public specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ret
	}

	.method public virtual instance int32 Number () cil managed
	{
		.maxstack 1
		ldc.i4.1
		ret
	}
}

.class public auto ansi TailCallsDerived extends TailCallsBase
{
	.method public specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void TailCallsBase::.ctor ()
		ret
	}

	.method public virtual instance int32 Number () cil managed
	{
		.maxstack 1
		ldc.i4.7
		ret
	}
}
