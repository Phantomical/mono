// CIL has a single floating point stack type, so a value produced as r4 may be
// stored where an r8 is kept and the other way round, and the store is what
// decides the precision. No C# compiler emits this, but it is legal IL, and an
// engine that keeps r4 and r8 apart internally has to convert at every store
// rather than copy the bytes across.
//
// Each case below puts a value of one width on the stack and stores it through
// a destination of the other, then reads it back. Getting it wrong reads four
// bytes of an eight byte value or eight bytes of a four byte one, so the
// failures are wild rather than off in the last place.
//
// r4 side uses 1.5, which both widths hold exactly. r8 side uses 1.1, which
// neither holds exactly and float32 holds differently, so a narrowing that did
// not happen is just as visible as one that read the wrong bytes.

.assembly extern mscorlib { }
.assembly 'float-stack-precision' { }
.module 'float-stack-precision.exe'

.class private auto ansi beforefieldinit FP extends [mscorlib]System.Object
{
  .field public static float64 s_r8
  .field public static float32 s_r4
  .field public        float64 i_r8
  .field public        float32 i_r4
  .field public static int32   failures

  .method public hidebysig specialname rtspecialname instance void .ctor () cil managed
  {
    .maxstack 8
    ldarg.0
    call instance void [mscorlib]System.Object::.ctor()
    ret
  }

  .method private hidebysig static void check (float64 got, float64 want, string what) cil managed
  {
    .maxstack 8
    ldarg.0
    ldarg.1
    beq.s      IL_OK
    ldstr      "FAIL "
    call       void [mscorlib]System.Console::Write(string)
    ldarg.2
    call       void [mscorlib]System.Console::Write(string)
    ldstr      ": got "
    call       void [mscorlib]System.Console::Write(string)
    ldarg.0
    call       void [mscorlib]System.Console::Write(float64)
    ldstr      " want "
    call       void [mscorlib]System.Console::Write(string)
    ldarg.1
    call       void [mscorlib]System.Console::WriteLine(float64)
    ldsfld     int32 FP::failures
    ldc.i4.1
    add
    stsfld     int32 FP::failures
  IL_OK:
    ret
  }

  // What (double) 1.1f is, without relying on the conversion under test.
  .method private hidebysig static float64 narrowed () cil managed
  {
    .maxstack 8
    ldc.r4     1.1
    conv.r8
    ret
  }

  // A float64 parameter reached with an r4 argument, and the reverse.
  .method private hidebysig static void arg_r8 (float64 v) cil managed
  {
    .maxstack 8
    ldarg.0
    ldc.r8     1.5
    ldstr      "call arg float64 <- r4"
    call       void FP::check(float64, float64, string)
    ret
  }

  .method private hidebysig static void arg_r4 (float32 v) cil managed
  {
    .maxstack 8
    ldarg.0
    conv.r8
    call       float64 FP::narrowed()
    ldstr      "call arg float32 <- r8"
    call       void FP::check(float64, float64, string)
    ret
  }

  .method private hidebysig static void arg_both (float64 v, float32 w) cil managed
  {
    .maxstack 8
    ldarg.0
    ldc.r8     1.5
    ldstr      "calli arg float64 <- r4"
    call       void FP::check(float64, float64, string)
    ldarg.1
    conv.r8
    call       float64 FP::narrowed()
    ldstr      "calli arg float32 <- r8"
    call       void FP::check(float64, float64, string)
    ret
  }

  // A float64 return reached with an r4 value, and the reverse.
  .method private hidebysig static float64 ret_r8 () cil managed
  {
    .maxstack 8
    ldc.r4     1.5
    ret
  }

  .method private hidebysig static float32 ret_r4 () cil managed
  {
    .maxstack 8
    ldc.r8     1.1
    ret
  }

  // starg into a parameter of the other width.
  .method private hidebysig static float64 starg_r8 (float64 v) cil managed
  {
    .maxstack 8
    ldc.r4     1.5
    starg.s    v
    ldarg.0
    ret
  }

  .method private hidebysig static float32 starg_r4 (float32 v) cil managed
  {
    .maxstack 8
    ldc.r8     1.1
    starg.s    v
    ldarg.0
    ret
  }

  // Two predecessors of one block, delivering different widths. Whichever the
  // block ends up reading its entry stack at, both paths have to arrive as that
  // one - so 1.5, which either width holds exactly, is the same answer both
  // ways round. Written twice so that each width gets to be the one that
  // reached the block first.
  .method private hidebysig static float64 merge_r4_first (bool path) cil managed
  {
    .maxstack 8
    ldc.r4     1.5
    ldarg.0
    brfalse.s  IL_JOIN
    pop
    ldc.r8     1.5
  IL_JOIN:
    box        [mscorlib]System.Double
    unbox.any  [mscorlib]System.Double
    ret
  }

  .method private hidebysig static float64 merge_r8_first (bool path) cil managed
  {
    .maxstack 8
    ldc.r8     1.5
    ldarg.0
    brfalse.s  IL_JOIN
    pop
    ldc.r4     1.5
  IL_JOIN:
    box        [mscorlib]System.Double
    unbox.any  [mscorlib]System.Double
    ret
  }

  .method public hidebysig static int32 Main () cil managed
  {
    .entrypoint
    .maxstack 8
    .locals init (float64 d8, float32 d4, float64[] a8, float32[] a4, class FP obj)

    newobj     instance void FP::.ctor()
    stloc.s    obj
    ldc.i4.1   newarr [mscorlib]System.Double  stloc.s a8
    ldc.i4.1   newarr [mscorlib]System.Single  stloc.s a4

    // box
    ldc.r4     1.5
    box        [mscorlib]System.Double
    unbox.any  [mscorlib]System.Double
    ldc.r8     1.5
    ldstr      "box Double <- r4"
    call       void FP::check(float64, float64, string)

    ldc.r8     1.1
    box        [mscorlib]System.Single
    unbox.any  [mscorlib]System.Single
    conv.r8
    call       float64 FP::narrowed()
    ldstr      "box Single <- r8"
    call       void FP::check(float64, float64, string)

    // stloc
    ldc.r4     1.5
    stloc.s    d8
    ldloc.s    d8
    ldc.r8     1.5
    ldstr      "stloc float64 <- r4"
    call       void FP::check(float64, float64, string)

    ldc.r8     1.1
    stloc.s    d4
    ldloc.s    d4
    conv.r8
    call       float64 FP::narrowed()
    ldstr      "stloc float32 <- r8"
    call       void FP::check(float64, float64, string)

    // starg
    ldc.r8     0.0
    call       float64 FP::starg_r8(float64)
    ldc.r8     1.5
    ldstr      "starg float64 <- r4"
    call       void FP::check(float64, float64, string)

    ldc.r4     0.0
    call       float32 FP::starg_r4(float32)
    conv.r8
    call       float64 FP::narrowed()
    ldstr      "starg float32 <- r8"
    call       void FP::check(float64, float64, string)

    // stsfld
    ldc.r4     1.5
    stsfld     float64 FP::s_r8
    ldsfld     float64 FP::s_r8
    ldc.r8     1.5
    ldstr      "stsfld float64 <- r4"
    call       void FP::check(float64, float64, string)

    ldc.r8     1.1
    stsfld     float32 FP::s_r4
    ldsfld     float32 FP::s_r4
    conv.r8
    call       float64 FP::narrowed()
    ldstr      "stsfld float32 <- r8"
    call       void FP::check(float64, float64, string)

    // stfld
    ldloc.s    obj
    ldc.r4     1.5
    stfld      float64 FP::i_r8
    ldloc.s    obj
    ldfld      float64 FP::i_r8
    ldc.r8     1.5
    ldstr      "stfld float64 <- r4"
    call       void FP::check(float64, float64, string)

    ldloc.s    obj
    ldc.r8     1.1
    stfld      float32 FP::i_r4
    ldloc.s    obj
    ldfld      float32 FP::i_r4
    conv.r8
    call       float64 FP::narrowed()
    ldstr      "stfld float32 <- r8"
    call       void FP::check(float64, float64, string)

    // stelem
    ldloc.s    a8
    ldc.i4.0
    ldc.r4     1.5
    stelem.r8
    ldloc.s    a8
    ldc.i4.0
    ldelem.r8
    ldc.r8     1.5
    ldstr      "stelem.r8 <- r4"
    call       void FP::check(float64, float64, string)

    ldloc.s    a4
    ldc.i4.0
    ldc.r8     1.1
    stelem.r4
    ldloc.s    a4
    ldc.i4.0
    ldelem.r4
    conv.r8
    call       float64 FP::narrowed()
    ldstr      "stelem.r4 <- r8"
    call       void FP::check(float64, float64, string)

    // stind
    ldloca.s   d8
    ldc.r4     1.5
    stind.r8
    ldloc.s    d8
    ldc.r8     1.5
    ldstr      "stind.r8 <- r4"
    call       void FP::check(float64, float64, string)

    ldloca.s   d4
    ldc.r8     1.1
    stind.r4
    ldloc.s    d4
    conv.r8
    call       float64 FP::narrowed()
    ldstr      "stind.r4 <- r8"
    call       void FP::check(float64, float64, string)

    // stobj
    ldloca.s   d8
    ldc.r4     1.5
    stobj      [mscorlib]System.Double
    ldloc.s    d8
    ldc.r8     1.5
    ldstr      "stobj Double <- r4"
    call       void FP::check(float64, float64, string)

    ldloca.s   d4
    ldc.r8     1.1
    stobj      [mscorlib]System.Single
    ldloc.s    d4
    conv.r8
    call       float64 FP::narrowed()
    ldstr      "stobj Single <- r8"
    call       void FP::check(float64, float64, string)

    // call arguments
    ldc.r4     1.5
    call       void FP::arg_r8(float64)
    ldc.r8     1.1
    call       void FP::arg_r4(float32)

    // calli, whose function pointer sits above the arguments rather than
    // being the last of them
    ldc.r4     1.5
    ldc.r8     1.1
    ldftn      void FP::arg_both(float64, float32)
    calli      void(float64, float32)

    // return values
    call       float64 FP::ret_r8()
    ldc.r8     1.5
    ldstr      "ret float64 <- r4"
    call       void FP::check(float64, float64, string)

    call       float32 FP::ret_r4()
    conv.r8
    call       float64 FP::narrowed()
    ldstr      "ret float32 <- r8"
    call       void FP::check(float64, float64, string)

    // block entry merges
    ldc.i4.0
    call       float64 FP::merge_r4_first(bool)
    ldc.r8     1.5
    ldstr      "merge, r4 first, r4 arriving"
    call       void FP::check(float64, float64, string)

    ldc.i4.1
    call       float64 FP::merge_r4_first(bool)
    ldc.r8     1.5
    ldstr      "merge, r4 first, r8 arriving"
    call       void FP::check(float64, float64, string)

    ldc.i4.0
    call       float64 FP::merge_r8_first(bool)
    ldc.r8     1.5
    ldstr      "merge, r8 first, r8 arriving"
    call       void FP::check(float64, float64, string)

    ldc.i4.1
    call       float64 FP::merge_r8_first(bool)
    ldc.r8     1.5
    ldstr      "merge, r8 first, r4 arriving"
    call       void FP::check(float64, float64, string)

    ldsfld     int32 FP::failures
    brtrue.s   IL_FAILED
    ldstr      "OK"
    call       void [mscorlib]System.Console::WriteLine(string)
    ldc.i4.0
    ret
  IL_FAILED:
    ldsfld     int32 FP::failures
    ret
  }
}
