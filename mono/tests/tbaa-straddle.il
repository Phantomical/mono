// A slot written as a reference and read as a scalar, in one body.
//
// This is what keeps ldind and stind off the `!tbaa` tree. `Unsafe.As ()` makes
// the shape from C#, and it is plain CIL: valid, unverifiable, and something the
// runtime runs.
//
// Straddle reads the slot as a native int, stores a reference into it, then
// reads it as a native int again. The second read has to see the store.
//
// It needs tier 2, where GVN runs. Straddle carries noinlining so the promotion
// reaches the body that holds the reads.
//
// This does not prove the tags are what keep the two reads apart. Tagging ldind
// and stind puts the reads on the scalar leaf and the store on the reference
// leaf, and the test still passes: a reference store drags the write barrier,
// whose volatile read of the collector's concurrent-collection flag blocks the
// forward whatever the tags say. Under Boehm the barrier is a call instead. So
// this covers the shape, and the rule that keeps ldind and stind untagged rests
// on ECMA-335 rather than on this test.

.assembly extern mscorlib { }
.assembly 'tbaa-straddle' { }

.class private abstract auto ansi sealed Mono.Tiering.MonoTier
	extends [mscorlib]System.Object
{
	.method public hidebysig static bool PromoteNow (native int, int32)
		cil managed internalcall
	{
	}
}

.class public Holder extends [mscorlib]System.Object
{
	.field public object Reference

	.method public hidebysig specialname rtspecialname instance void .ctor () cil managed
	{
		.maxstack 1
		ldarg.0
		call instance void [mscorlib]System.Object::.ctor ()
		ret
	}
}

.class public Tests extends [mscorlib]System.Object
{
	.method public static int32 Main () cil managed
	{
		.entrypoint
		.maxstack 4
		.locals init (class Holder h, object o,
		              valuetype [mscorlib]System.RuntimeMethodHandle handle)

		newobj instance void Holder::.ctor ()
		stloc.0
		newobj instance void [mscorlib]System.Object::.ctor ()
		stloc.1

		// MonoTier::tier2 is 3 (mono/mini/domain-method.hpp).
		ldtoken method int32 Tests::Straddle (class Holder, object)
		stloc.2
		ldloca.s handle
		call instance native int [mscorlib]System.RuntimeMethodHandle::get_Value ()
		ldc.i4.3
		call bool Mono.Tiering.MonoTier::PromoteNow (native int, int32)
		brtrue.s promoted

		ldstr "FAIL: Straddle would not compile at tier 2"
		call void [mscorlib]System.Console::WriteLine (string)
		ldc.i4.1
		ret

	promoted:
		// The slot starts null, so a read that sees the store has to differ
		// from the one before it.
		ldloc.0
		ldloc.1
		call int32 Tests::Straddle (class Holder, object)
		brtrue.s ok

		ldstr "FAIL: the second read was answered from the first"
		call void [mscorlib]System.Console::WriteLine (string)
		ldc.i4.1
		ret

	ok:
		ldstr "OK"
		call void [mscorlib]System.Console::WriteLine (string)
		ldc.i4.0
		ret
	}

	// Returns the slot read as a native int after the store, less the same read
	// before it. Zero means the second read was answered from the first.
	.method public static int32 Straddle (class Holder h, object o)
		cil managed noinlining
	{
		.maxstack 4
		.locals init (native int before, native int after)

		ldarg.0
		ldflda object Holder::Reference
		ldind.i
		stloc.0

		ldarg.0
		ldflda object Holder::Reference
		ldarg.1
		stind.ref

		ldarg.0
		ldflda object Holder::Reference
		ldind.i
		stloc.1

		ldloc.1
		ldloc.0
		sub
		conv.i4
		ret
	}
}
