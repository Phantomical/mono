// A vector constructs through newobj as well as through newarr, and both take
// the collector's array allocator. C# writes only the newarr form, so this
// corpus is the only exerciser of the other one.

.assembly extern mscorlib
{
  .ver 4:0:0:0
}

.assembly 'vector-newobj'
{
  .hash algorithm 0x00008004
  .ver 0:0:0:0
}

.module 'vector-newobj.exe'

.class public auto ansi beforefieldinit Driver
       extends [mscorlib]System.Object
{
  .method public static int32 MakeLength (int32 n) cil managed
  {
    .maxstack 2
    ldarg.0
    newobj instance void int32[]::.ctor(int32)
    ldlen
    conv.i4
    ret
  }

  // Answers 1 when a negative length draws OverflowException, which is what
  // ECMA-335 asks for and what the runtime's own array-new icall answers.
  .method public static int32 RefusesNegative () cil managed
  {
    .maxstack 2
    .locals init (int32 V_0)
    .try
    {
      ldc.i4.m1
      newobj instance void int32[]::.ctor(int32)
      pop
      ldc.i4.0
      stloc.0
      leave.s DONE
    }
    catch [mscorlib]System.OverflowException
    {
      pop
      ldc.i4.1
      stloc.0
      leave.s DONE
    }
  DONE:
    ldloc.0
    ret
  }

  .method public static int32 Check () cil managed
  {
    .maxstack 3
    .locals init (int32[] V_0)

    ldc.i4.5
    call int32 Driver::MakeLength(int32)
    ldc.i4.5
    beq.s L1
    ldc.i4.1
    ret
  L1:
    ldc.i4.0
    call int32 Driver::MakeLength(int32)
    brfalse.s L2
    ldc.i4.2
    ret
  L2:
    call int32 Driver::RefusesNegative()
    brtrue.s L3
    ldc.i4.3
    ret
  L3:
    ldc.i4.4
    newobj instance void int32[]::.ctor(int32)
    stloc.0

    // A fresh vector reads back zero everywhere and keeps what a store puts
    // in it. A short allocation shows up here.
    ldloc.0
    ldc.i4.3
    ldelem.i4
    brfalse.s L4
    ldc.i4.4
    ret
  L4:
    ldloc.0
    ldc.i4.3
    ldc.i4 42
    stelem.i4
    ldloc.0
    ldc.i4.3
    ldelem.i4
    ldc.i4 42
    beq.s L5
    ldc.i4.5
    ret
  L5:
    ldc.i4.0
    ret
  }

  .method public static int32 Main () cil managed
  {
    .entrypoint
    .maxstack 3
    .locals init (int32 V_0, int32 V_1)

    // Enough calls that every method above leaves tier 0 and the loop then
    // runs against compiled bodies.
    ldc.i4.0
    stloc.0
  LOOP:
    call int32 Driver::Check()
    stloc.1
    ldloc.1
    brfalse.s NEXT
    ldloc.1
    ret
  NEXT:
    ldloc.0
    ldc.i4.1
    add
    stloc.0
    ldloc.0
    ldc.i4 20000
    blt LOOP

    ldc.i4.0
    ret
  }
}
