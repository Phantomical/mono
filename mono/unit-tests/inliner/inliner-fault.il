// inliner-fault.il - a single try/fault method for inliner-tests.cs.
//
// C# has no syntax for a `fault` clause (it only exposes catch/finally), so this
// shape - a handler that runs on the exceptional path only, then always rethrows -
// has to come from hand-written IL, same convention as generics-variant-types.il.

.assembly extern mscorlib
{
	.publickeytoken = (B7 7A 5C 56 19 34 E0 89 )
	.ver 4:0:0:0
}
.assembly 'inliner-fault'
{
	.hash algorithm 0x00008004
	.ver 0:0:0:0
}
.module 'inliner-fault.dll'
.imagebase 0x00400000
.file alignment 0x00000200
.stackreserve 0x00100000
.subsystem 0x0003
.corflags 0x00000001

.namespace MonoTests.Inliner {
	.class public abstract auto ansi sealed beforefieldinit FaultHelpers extends [mscorlib]System.Object
	{
		.field public static int32 FaultRunCount

		// Runs the fault handler (bumping FaultRunCount) iff X < 0, in which case
		// the exception propagates to the caller; otherwise returns X * 2 and the
		// fault handler never runs.
		.method public hidebysig static int32 TryFault (int32 x) cil managed
		{
			.maxstack 3
			.locals init (int32 result)

			.try
			{
				ldarg.0
				ldc.i4.0
				blt.s THROWIT

				ldarg.0
				ldc.i4.2
				mul
				stloc.0
				leave.s DONE

			THROWIT:
				newobj instance void [mscorlib]System.InvalidOperationException::.ctor()
				throw
			}
			fault
			{
				ldsfld int32 MonoTests.Inliner.FaultHelpers::FaultRunCount
				ldc.i4.1
				add
				stsfld int32 MonoTests.Inliner.FaultHelpers::FaultRunCount
				endfault
			}

		DONE:
			ldloc.0
			ret
		}
	}
}
