// Exception handling: the landing pads, and the leave -> finally-chain ->
// endfinally switch that carries a leave through the handlers it crosses.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'eh' { .hash algorithm 0x00008004 .ver 0:0:0:0 }
.module 'eh.dll'

.class public abstract auto ansi sealed beforefieldinit Eh extends [mscorlib]System.Object
{
	.method public static int32 TryCatch (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int32 result, class [mscorlib]System.Exception ex)
		.try
		{
			ldarg.0
			ldc.i4.2
			div
			stloc.0
			leave.s DONE
		}
		catch [mscorlib]System.Exception
		{
			stloc.1
			ldc.i4.m1
			stloc.0
			leave.s DONE
		}
	DONE:
		ldloc.0
		ret
	}

	.method public static int32 TwoCatches (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int32 result, class [mscorlib]System.Exception ex)
		.try
		{
			ldarg.0
			ldc.i4.2
			div
			stloc.0
			leave.s DONE
		}
		catch [mscorlib]System.DivideByZeroException
		{
			stloc.1
			ldc.i4.m1
			stloc.0
			leave.s DONE
		}
		catch [mscorlib]System.Exception
		{
			stloc.1
			ldc.i4.s -2
			stloc.0
			leave.s DONE
		}
	DONE:
		ldloc.0
		ret
	}

	.method public static int32 TryFinally (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int32 result)
		.try
		{
			ldarg.0
			ldc.i4.1
			add
			stloc.0
			leave.s DONE
		}
		finally
		{
			ldloc.0
			ldc.i4.2
			mul
			stloc.0
			endfinally
		}
	DONE:
		ldloc.0
		ret
	}

	// Two leaves out of one try, to two different targets: the finally's
	// endfinally has to switch on which one is in progress.
	.method public static int32 TwoLeaves (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int32 result)
		.try
		{
			ldarg.0
			brtrue.s OTHER_WAY
			ldc.i4.s 10
			stloc.0
			leave.s DONE
		OTHER_WAY:
			ldc.i4.s 20
			stloc.0
			leave.s ELSEWHERE
		}
		finally
		{
			ldloc.0
			ldc.i4.1
			add
			stloc.0
			endfinally
		}
	ELSEWHERE:
		ldloc.0
		ldc.i4.s 100
		add
		ret
	DONE:
		ldloc.0
		ret
	}

	// One finally with two endfinallys — the shape a C# `using` compiles to,
	// one for the disposable path and one for the null one. Either can be the
	// one reached, so both have to carry the leave on to its continuation.
	.method public static int32 TwoEndfinallys (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int32 result)
		.try
		{
			ldarg.0
			stloc.0
			leave.s DONE
		}
		finally
		{
			ldloc.0
			brfalse.s WAS_ZERO
			ldloc.0
			ldc.i4.1
			add
			stloc.0
			endfinally
		WAS_ZERO:
			ldc.i4.s 99
			stloc.0
			endfinally
		}
	DONE:
		ldloc.0
		ret
	}

	// A leave out of the inner try crosses two finallys on the way out.
	.method public static int32 NestedFinally (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int32 result)
		.try
		{
			.try
			{
				ldarg.0
				stloc.0
				leave.s DONE
			}
			finally
			{
				ldloc.0
				ldc.i4.1
				add
				stloc.0
				endfinally
			}
		}
		finally
		{
			ldloc.0
			ldc.i4.2
			mul
			stloc.0
			endfinally
		}
	DONE:
		ldloc.0
		ret
	}

	.method public static int32 CatchInsideFinally (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int32 result, class [mscorlib]System.Exception ex)
		.try
		{
			ldarg.0
			stloc.0
			leave.s DONE
		}
		finally
		{
			.try
			{
				ldloc.0
				ldc.i4.1
				add
				stloc.0
				leave.s OUT
			}
			catch [mscorlib]System.Exception
			{
				stloc.1
				leave.s OUT
			}
		OUT:
			endfinally
		}
	DONE:
		ldloc.0
		ret
	}

	.method public static void Throw (class [mscorlib]System.Exception e) cil managed
	{
		.maxstack 1
		ldarg.0
		throw
	}

	// A call protected by two nested clauses at once: its landing pad has to
	// carry both, since the runtime re-enters through it for either.
	.method public static int32 CallUnderCatchAndFinally (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int32 result, class [mscorlib]System.Exception ex)
		.try
		{
			.try
			{
				ldarg.0
				call int32 Eh::CallInTry (int32)
				stloc.0
				leave.s DONE
			}
			catch [mscorlib]System.Exception
			{
				stloc.1
				ldc.i4.m1
				stloc.0
				leave.s DONE
			}
		}
		finally
		{
			ldloc.0
			ldc.i4.1
			add
			stloc.0
			endfinally
		}
	DONE:
		ldloc.0
		ret
	}

	.method public static int32 Rethrow (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int32 result, class [mscorlib]System.Exception ex)
		.try
		{
			ldarg.0
			stloc.0
			leave.s DONE
		}
		catch [mscorlib]System.Exception
		{
			stloc.1
			rethrow
		}
	DONE:
		ldloc.0
		ret
	}

	// A call inside a try has to be an invoke, so that it can unwind to the
	// handler rather than past it.
	.method public static int32 CallInTry (int32 x) cil managed
	{
		.maxstack 2
		.locals init (int32 result, class [mscorlib]System.Exception ex)
		.try
		{
			ldarg.0
			call int32 Eh::Helper (int32)
			stloc.0
			leave.s DONE
		}
		catch [mscorlib]System.Exception
		{
			stloc.1
			ldc.i4.m1
			stloc.0
			leave.s DONE
		}
	DONE:
		ldloc.0
		ret
	}

	.method public static int32 Helper (int32 x) cil managed
	{
		.maxstack 1
		ldarg.0
		ret
	}
}
