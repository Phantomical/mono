// Float opcodes whose interpreted form disagreed with the compiled one.
//
// ckfinite hands its operand back, so a single has to come back a single. The
// interpreter read every operand as a double, which puts four bytes of whatever
// followed the value into the result.
//
// conv.ovf.i2.un did not step over itself, so the dispatch loop ran it again
// with the same ip and the method never returned.
//
// Each case is called once, so it runs interpreted. The compiled answers are the
// ones asserted here.

.assembly extern mscorlib { }
.assembly 'interp-float-conv' { }
.module 'interp-float-conv.exe'

.class public auto ansi beforefieldinit Test extends [mscorlib]System.Object {

  .method public static float32 CkFiniteR4 (float32 f) cil managed noinlining {
    .maxstack 2
    ldarg.0
    ckfinite
    ret
  }

  .method public static float64 CkFiniteR8 (float64 d) cil managed noinlining {
    .maxstack 2
    ldarg.0
    ckfinite
    ret
  }

  .method public static int32 OvfI2Un (float64 d) cil managed noinlining {
    .maxstack 2
    ldarg.0
    conv.ovf.i2.un
    ret
  }

  .method public static int32 OvfI1Un (float64 d) cil managed noinlining {
    .maxstack 2
    ldarg.0
    conv.ovf.i1.un
    ret
  }

  .method public static int32 OvfI4Un (float64 d) cil managed noinlining {
    .maxstack 2
    ldarg.0
    conv.ovf.i4.un
    ret
  }

  .method private static void Fail (string what) cil managed noinlining {
    .maxstack 2
    ldstr "FAILED: "
    ldarg.0
    call string [mscorlib]System.String::Concat(string, string)
    call void [mscorlib]System.Console::WriteLine(string)
    ret
  }

  .method public static int32 Main () cil managed {
    .entrypoint
    .maxstack 4
    .locals init (int32 bad)

    ldc.i4.0
    stloc.0

    // A single must survive ckfinite unchanged.
    ldc.r4 1.5
    call float32 Test::CkFiniteR4 (float32)
    ldc.r4 1.5
    beq CK8
    ldstr "ckfinite over a single"
    call void Test::Fail (string)
    ldc.i4.1
    stloc.0

  CK8:
    ldc.r8 2.5
    call float64 Test::CkFiniteR8 (float64)
    ldc.r8 2.5
    beq CKNAN
    ldstr "ckfinite over a double"
    call void Test::Fail (string)
    ldc.i4.1
    stloc.0

    // A single that is not finite still has to raise.
  CKNAN:
    .try {
      ldc.r4 float32(0x7FC00000)
      call float32 Test::CkFiniteR4 (float32)
      pop
      ldstr "ckfinite over a NaN single did not raise"
      call void Test::Fail (string)
      ldc.i4.1
      stloc.0
      leave OVF
    }
    catch [mscorlib]System.ArithmeticException {
      pop
      leave OVF
    }

    // Each of these has to return rather than run forever.
  OVF:
    ldc.r8 3.0
    call int32 Test::OvfI2Un (float64)
    ldc.i4.3
    beq OVF1
    ldstr "conv.ovf.i2.un"
    call void Test::Fail (string)
    ldc.i4.1
    stloc.0

  OVF1:
    ldc.r8 4.0
    call int32 Test::OvfI1Un (float64)
    ldc.i4.4
    beq OVF4
    ldstr "conv.ovf.i1.un"
    call void Test::Fail (string)
    ldc.i4.1
    stloc.0

  OVF4:
    ldc.r8 5.0
    call int32 Test::OvfI4Un (float64)
    ldc.i4.5
    beq DONE
    ldstr "conv.ovf.i4.un"
    call void Test::Fail (string)
    ldc.i4.1
    stloc.0

  DONE:
    ldloc.0
    ret
  }
}
