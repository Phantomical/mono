// Exception handling clauses C# does not emit: a fault handler, a filter that
// runs while the frames below it are still live, and leave out of nested
// protected blocks.

.assembly extern mscorlib { }
.assembly 'eh-il' { }

.class public auto ansi EhIL extends [mscorlib]System.Object
{
	.field public static int32 marks

	// A fault handler runs when an exception leaves the try and not otherwise.
	.method public static int32 test_3_fault_runs_on_exception () cil managed
	{
		.maxstack 2
		ldc.i4.0
		stsfld int32 EhIL::marks
		.try {
			.try {
				newobj instance void [mscorlib]System.InvalidOperationException::.ctor ()
				throw
			} fault {
				ldsfld int32 EhIL::marks
				ldc.i4.3
				add
				stsfld int32 EhIL::marks
				endfault
			}
		} catch [mscorlib]System.InvalidOperationException {
			pop
			leave IL_done
		}
	IL_done:
		ldsfld int32 EhIL::marks
		ret
	}

	.method public static int32 test_0_fault_skipped_on_success () cil managed
	{
		.maxstack 2
		ldc.i4.0
		stsfld int32 EhIL::marks
		.try {
			leave IL_done
		} fault {
			ldc.i4.7
			stsfld int32 EhIL::marks
			endfault
		}
	IL_done:
		ldsfld int32 EhIL::marks
		ret
	}

	.method public static int32 test_5_finally_on_leave () cil managed
	{
		.maxstack 2
		ldc.i4.0
		stsfld int32 EhIL::marks
		.try {
			leave IL_done
		} finally {
			ldc.i4.5
			stsfld int32 EhIL::marks
			endfinally
		}
	IL_done:
		ldsfld int32 EhIL::marks
		ret
	}

	// Three protected blocks unwound by one exception.  Each handler adds a mark
	// of its own size, so the total says which ran.
	.method public static int32 test_111_nested_finallys () cil managed
	{
		.maxstack 3
		ldc.i4.0
		stsfld int32 EhIL::marks
		.try {
			.try {
				.try {
					newobj instance void [mscorlib]System.InvalidOperationException::.ctor ()
					throw
				} finally {
					ldsfld int32 EhIL::marks
					ldc.i4.1
					add
					stsfld int32 EhIL::marks
					endfinally
				}
			} finally {
				ldsfld int32 EhIL::marks
				ldc.i4.s 10
				add
				stsfld int32 EhIL::marks
				endfinally
			}
		} catch [mscorlib]System.InvalidOperationException {
			pop
			ldsfld int32 EhIL::marks
			ldc.i4.s 100
			add
			stsfld int32 EhIL::marks
			leave IL_done
		}
	IL_done:
		ldsfld int32 EhIL::marks
		ret
	}

	// The filter reads a field the finally below it overwrites, so the answer
	// says the filter ran first.
	.method public static int32 test_2_filter_runs_before_finally () cil managed
	{
		.maxstack 3
		ldc.i4.1
		stsfld int32 EhIL::marks
		.try {
			.try {
				newobj instance void [mscorlib]System.InvalidOperationException::.ctor ()
				throw
			} finally {
				ldc.i4.s 9
				stsfld int32 EhIL::marks
				endfinally
			}
		} filter {
			pop
			ldsfld int32 EhIL::marks
			ldc.i4.1
			ceq
			endfilter
		} {
			pop
			ldc.i4.2
			stsfld int32 EhIL::marks
			leave IL_done
		}
	IL_done:
		ldsfld int32 EhIL::marks
		ret
	}

	.method public static int32 test_6_leave_from_catch_runs_finally () cil managed
	{
		.maxstack 3
		ldc.i4.0
		stsfld int32 EhIL::marks
		.try {
			.try {
				newobj instance void [mscorlib]System.InvalidOperationException::.ctor ()
				throw
			} catch [mscorlib]System.InvalidOperationException {
				pop
				ldc.i4.2
				stsfld int32 EhIL::marks
				leave IL_done
			}
		} finally {
			ldsfld int32 EhIL::marks
			ldc.i4.4
			add
			stsfld int32 EhIL::marks
			endfinally
		}
	IL_done:
		ldsfld int32 EhIL::marks
		ret
	}

	.method public static int32 test_1_rethrow () cil managed
	{
		.maxstack 2
		.try {
			.try {
				newobj instance void [mscorlib]System.InvalidOperationException::.ctor ()
				throw
			} catch [mscorlib]System.InvalidOperationException {
				pop
				rethrow
			}
		} catch [mscorlib]System.InvalidOperationException {
			pop
			ldc.i4.1
			ret
		}
	}
}
