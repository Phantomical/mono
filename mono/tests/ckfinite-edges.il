// ckfinite over the values that sit on the boundary it draws, at both widths.
//
// ckfiniteTest.il reaches +infinity and a NaN at float64 and nothing else, so
// -infinity, every float32 case and the sign of a zero were never asserted.
// Each is a value the lowering has to tell apart from its neighbour rather
// than from an ordinary number. The smallest denormal and the largest finite
// carry the extreme exponents an operand can have and still be finite. And -0
// is what a lowering that answered with its own arithmetic, rather than with
// the operand, would hand back as +0.
//
// Every value crosses a call boundary at its declared width, so the float32
// cases stay float32 rather than reaching a float64 test through a conversion.

.assembly extern mscorlib { }
.assembly 'ckfinite-edges' { }
.module 'ckfinite-edges.exe'

.class public auto ansi beforefieldinit Test extends [mscorlib]System.Object {

  .field private static int32 bad

  .method public static float32 CkR4 (float32 f) cil managed noinlining {
    .maxstack 2
    ldarg.0
    ckfinite
    ret
  }

  .method public static float64 CkR8 (float64 d) cil managed noinlining {
    .maxstack 2
    ldarg.0
    ckfinite
    ret
  }

  // 1 if ckfinite raised, 0 if it handed the value back.
  .method private static int32 RaisesR4 (float32 f) cil managed noinlining {
    .maxstack 2
    .locals init (int32 raised)
    ldc.i4.0
    stloc.0
    .try {
      ldarg.0
      call float32 Test::CkR4 (float32)
      pop
      leave L_done
    }
    catch [mscorlib]System.ArithmeticException {
      pop
      ldc.i4.1
      stloc.0
      leave L_done
    }
  L_done:
    ldloc.0
    ret
  }

  .method private static int32 RaisesR8 (float64 d) cil managed noinlining {
    .maxstack 2
    .locals init (int32 raised)
    ldc.i4.0
    stloc.0
    .try {
      ldarg.0
      call float64 Test::CkR8 (float64)
      pop
      leave L_done
    }
    catch [mscorlib]System.ArithmeticException {
      pop
      ldc.i4.1
      stloc.0
      leave L_done
    }
  L_done:
    ldloc.0
    ret
  }

  .method private static int64 CheckedBitsR4 (float32 f) cil managed noinlining {
    .maxstack 2
    ldarg.0
    call float32 Test::CkR4 (float32)
    conv.r8
    call int64 [mscorlib]System.BitConverter::DoubleToInt64Bits(float64)
    ret
  }

  .method private static int64 RawBitsR4 (float32 f) cil managed noinlining {
    .maxstack 2
    ldarg.0
    conv.r8
    call int64 [mscorlib]System.BitConverter::DoubleToInt64Bits(float64)
    ret
  }

  .method private static int64 CheckedBitsR8 (float64 d) cil managed noinlining {
    .maxstack 2
    ldarg.0
    call float64 Test::CkR8 (float64)
    call int64 [mscorlib]System.BitConverter::DoubleToInt64Bits(float64)
    ret
  }

  .method private static int64 RawBitsR8 (float64 d) cil managed noinlining {
    .maxstack 2
    ldarg.0
    call int64 [mscorlib]System.BitConverter::DoubleToInt64Bits(float64)
    ret
  }

  .method private static void Want (string what, int32 got, int32 wanted) cil managed noinlining {
    .maxstack 4
    ldarg.1
    ldarg.2
    beq L_ok
    ldstr "FAILED: "
    ldarg.0
    call string [mscorlib]System.String::Concat(string, string)
    call void [mscorlib]System.Console::WriteLine(string)
    ldsfld int32 Test::bad
    ldc.i4.1
    add
    stsfld int32 Test::bad
  L_ok:
    ret
  }

  .method private static void WantBits (string what, int64 got, int64 wanted) cil managed noinlining {
    .maxstack 4
    ldarg.1
    ldarg.2
    beq L_ok
    ldstr "FAILED: "
    ldarg.0
    call string [mscorlib]System.String::Concat(string, string)
    call void [mscorlib]System.Console::WriteLine(string)
    ldsfld int32 Test::bad
    ldc.i4.1
    add
    stsfld int32 Test::bad
  L_ok:
    ret
  }

  .method public static int32 Main () cil managed {
    .entrypoint
    .maxstack 4

    ldc.i4.0
    stsfld int32 Test::bad

    // float32, the values ckfinite has to reject.
    ldstr "r4 +inf"
    ldc.r4 float32(0x7F800000)
    call int32 Test::RaisesR4 (float32)
    ldc.i4.1
    call void Test::Want (string, int32, int32)

    ldstr "r4 -inf"
    ldc.r4 float32(0xFF800000)
    call int32 Test::RaisesR4 (float32)
    ldc.i4.1
    call void Test::Want (string, int32, int32)

    ldstr "r4 quiet NaN"
    ldc.r4 float32(0x7FC00000)
    call int32 Test::RaisesR4 (float32)
    ldc.i4.1
    call void Test::Want (string, int32, int32)

    ldstr "r4 signalling NaN"
    ldc.r4 float32(0x7F800001)
    call int32 Test::RaisesR4 (float32)
    ldc.i4.1
    call void Test::Want (string, int32, int32)

    // float32, the values it has to hand back.
    ldstr "r4 +0"
    ldc.r4 float32(0x00000000)
    call int32 Test::RaisesR4 (float32)
    ldc.i4.0
    call void Test::Want (string, int32, int32)

    ldstr "r4 -0"
    ldc.r4 float32(0x80000000)
    call int32 Test::RaisesR4 (float32)
    ldc.i4.0
    call void Test::Want (string, int32, int32)

    ldstr "r4 1.5"
    ldc.r4 1.5
    call int32 Test::RaisesR4 (float32)
    ldc.i4.0
    call void Test::Want (string, int32, int32)

    ldstr "r4 largest finite"
    ldc.r4 float32(0x7F7FFFFF)
    call int32 Test::RaisesR4 (float32)
    ldc.i4.0
    call void Test::Want (string, int32, int32)

    ldstr "r4 smallest denormal"
    ldc.r4 float32(0x00000001)
    call int32 Test::RaisesR4 (float32)
    ldc.i4.0
    call void Test::Want (string, int32, int32)

    // float64, the same two halves.
    ldstr "r8 +inf"
    ldc.r8 float64(0x7FF0000000000000)
    call int32 Test::RaisesR8 (float64)
    ldc.i4.1
    call void Test::Want (string, int32, int32)

    ldstr "r8 -inf"
    ldc.r8 float64(0xFFF0000000000000)
    call int32 Test::RaisesR8 (float64)
    ldc.i4.1
    call void Test::Want (string, int32, int32)

    ldstr "r8 quiet NaN"
    ldc.r8 float64(0x7FF8000000000000)
    call int32 Test::RaisesR8 (float64)
    ldc.i4.1
    call void Test::Want (string, int32, int32)

    ldstr "r8 signalling NaN"
    ldc.r8 float64(0x7FF0000000000001)
    call int32 Test::RaisesR8 (float64)
    ldc.i4.1
    call void Test::Want (string, int32, int32)

    ldstr "r8 +0"
    ldc.r8 float64(0x0000000000000000)
    call int32 Test::RaisesR8 (float64)
    ldc.i4.0
    call void Test::Want (string, int32, int32)

    ldstr "r8 -0"
    ldc.r8 float64(0x8000000000000000)
    call int32 Test::RaisesR8 (float64)
    ldc.i4.0
    call void Test::Want (string, int32, int32)

    ldstr "r8 2.5"
    ldc.r8 2.5
    call int32 Test::RaisesR8 (float64)
    ldc.i4.0
    call void Test::Want (string, int32, int32)

    ldstr "r8 largest finite"
    ldc.r8 float64(0x7FEFFFFFFFFFFFFF)
    call int32 Test::RaisesR8 (float64)
    ldc.i4.0
    call void Test::Want (string, int32, int32)

    ldstr "r8 smallest denormal"
    ldc.r8 float64(0x0000000000000001)
    call int32 Test::RaisesR8 (float64)
    ldc.i4.0
    call void Test::Want (string, int32, int32)

    // III.3.24: ckfinite returns its operand unchanged. -0 is what an answer
    // computed rather than forwarded loses.
    ldstr "r4 -0 handed back"
    ldc.r4 float32(0x80000000)
    call int64 Test::CheckedBitsR4 (float32)
    ldc.r4 float32(0x80000000)
    call int64 Test::RawBitsR4 (float32)
    call void Test::WantBits (string, int64, int64)

    ldstr "r4 largest finite handed back"
    ldc.r4 float32(0x7F7FFFFF)
    call int64 Test::CheckedBitsR4 (float32)
    ldc.r4 float32(0x7F7FFFFF)
    call int64 Test::RawBitsR4 (float32)
    call void Test::WantBits (string, int64, int64)

    ldstr "r4 smallest denormal handed back"
    ldc.r4 float32(0x00000001)
    call int64 Test::CheckedBitsR4 (float32)
    ldc.r4 float32(0x00000001)
    call int64 Test::RawBitsR4 (float32)
    call void Test::WantBits (string, int64, int64)

    ldstr "r8 -0 handed back"
    ldc.r8 float64(0x8000000000000000)
    call int64 Test::CheckedBitsR8 (float64)
    ldc.r8 float64(0x8000000000000000)
    call int64 Test::RawBitsR8 (float64)
    call void Test::WantBits (string, int64, int64)

    ldstr "r8 largest finite handed back"
    ldc.r8 float64(0x7FEFFFFFFFFFFFFF)
    call int64 Test::CheckedBitsR8 (float64)
    ldc.r8 float64(0x7FEFFFFFFFFFFFFF)
    call int64 Test::RawBitsR8 (float64)
    call void Test::WantBits (string, int64, int64)

    ldstr "r8 smallest denormal handed back"
    ldc.r8 float64(0x0000000000000001)
    call int64 Test::CheckedBitsR8 (float64)
    ldc.r8 float64(0x0000000000000001)
    call int64 Test::RawBitsR8 (float64)
    call void Test::WantBits (string, int64, int64)

    ldsfld int32 Test::bad
    ret
  }
}
