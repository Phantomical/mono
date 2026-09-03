// conv.u applied to a double: the one opcode-emulation shape
// tier0-classic.cs cannot reach in C#. No BCL type has an explicit
// conversion from float64 to a native-int-sized type, so csc/mcs never
// emits it. ClassicExerciseFpConvU's name carries the ClassicExercise
// substring runtime-suites.cmake already filters tier0-classic.cs's suites
// on, so the same run reaches this method too.
//
// Main calls it through MethodInfo.Invoke rather than a direct IL call, the
// same reflection route tier0-classic.cs's own Main takes to reach its
// filtered method. A direct call from an interpreted caller never asks the
// backend for its callee, so it never reaches the filter.

.assembly extern mscorlib { }
.assembly 'tier0-classic-fpconv' { }
.module 'tier0-classic-fpconv.exe'

.class public auto ansi beforefieldinit Test extends [mscorlib]System.Object {

  // conv.u8 only widens the native-uint conv.u already produced, so the
  // opcode under test is the first of the two.
  .method public static uint64 ClassicExerciseFpConvU (float64 d) cil managed noinlining {
    .maxstack 2
    ldarg.0
    conv.u
    conv.u8
    ret
  }

  .method public static int32 Main () cil managed {
    .entrypoint
    .maxstack 4
    .locals init (
      class [mscorlib]System.Reflection.MethodInfo m,
      object[] args,
      uint64 v)

    ldtoken Test
    call class [mscorlib]System.Type [mscorlib]System.Type::GetTypeFromHandle(valuetype [mscorlib]System.RuntimeTypeHandle)
    ldstr "ClassicExerciseFpConvU"
    callvirt instance class [mscorlib]System.Reflection.MethodInfo [mscorlib]System.Type::GetMethod(string)
    stloc.0

    ldc.i4.1
    newarr [mscorlib]System.Object
    dup
    ldc.i4.0
    ldc.r8 42.0
    box [mscorlib]System.Double
    stelem.ref
    stloc.1

    ldloc.0
    ldnull
    ldloc.1
    callvirt instance object [mscorlib]System.Reflection.MethodInfo::Invoke(object, object[])
    unbox.any [mscorlib]System.UInt64
    stloc.2

    ldloc.2
    ldc.i8 42
    beq OK

    ldstr "FAIL: conv.u from a double"
    call void [mscorlib]System.Console::WriteLine(string)
    ldc.i4.1
    ret

  OK:
    ldstr "OK"
    call void [mscorlib]System.Console::WriteLine(string)
    ldc.i4.0
    ret
  }
}
