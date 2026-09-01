// A compiled frame takes a method's address and an interpreted frame calls
// through it. A method's published address is its stub, and a stub is registered
// as a trampoline, so what this covers is the interpreter recovering the method
// from a trampoline record rather than refusing one.
//
// Which frame runs where is pinned rather than left to promotion: the consumer
// is the only method --llvm-opt=-mono-tier0-filter names, so it interprets
// while the producer around it compiles. No threshold produces that pair,
// because a producer is called at least as often as the method that calls it.

.assembly extern mscorlib { }
.assembly 'bug-unity-2' { }
.module 'bug-unity-2.exe'

.class public auto ansi beforefieldinit Test extends [mscorlib]System.Object {

  .method public static int32 Target (int32 a) cil managed noinlining {
    .maxstack 2
    ldarg.0
    ldc.i4.1
    add
    ret
  }

  // Compiled, so its ldftn hands out the stub rather than an interpreter entry.
  .method public static native int Produce () cil managed noinlining {
    .maxstack 1
    ldftn int32 Test::Target (int32)
    ret
  }

  // Interpreted, so the calli goes through MINT_CALLI.
  .method public static int32 CallInterpMe (native int p) cil managed noinlining {
    .maxstack 2
    ldc.i4 41
    ldarg.0
    calli int32 (int32)
    ret
  }

  .method public static int32 Main () cil managed {
    .entrypoint
    .maxstack 3
    .locals init (native int p, int32 r)

    call native int Test::Produce ()
    stloc.0

    ldloc.0
    call int32 Test::CallInterpMe (native int)
    stloc.1

    ldloc.1
    ldc.i4 42
    beq OK

    ldstr "calli through a compiled method's address returned "
    ldloc.1
    box [mscorlib]System.Int32
    call string [mscorlib]System.String::Concat(object, object)
    call void [mscorlib]System.Console::WriteLine(string)
    ldc.i4.1
    ret

  OK:
    ldc.i4.0
    ret
  }
}
