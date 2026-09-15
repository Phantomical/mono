// Type.IsClass must not lay its class out.
//
// A layout resolves every field's type, and a field whose assembly is not
// loaded yet records a failure the class then keeps for the rest of the
// process -- loading that assembly afterwards does not redeem it. So a
// failure here is a type that a host has finished off by asking a question
// about it too early.
//
// Written in IL because C# cannot name an assembly that is not there.

.assembly extern mscorlib
{
  .ver 4:0:0:0
  .publickeytoken = (B7 7A 5C 56 19 34 E0 89)
}

// Deliberately not built, and not resolvable at run time.
.assembly extern MonoTestsMissingDependency
{
  .ver 1:0:0:0
}

.assembly 'typeload-isclass-no-layout'
{
  .hash algorithm 0x00008004
  .ver 0:0:0:0
}
.module 'typeload-isclass-no-layout.exe'

.class public auto ansi beforefieldinit HoldsAMissingType
       extends [mscorlib]System.Object
{
  .field public class [MonoTestsMissingDependency]Missing.Dependency held

  .method public hidebysig specialname rtspecialname
          instance void .ctor () cil managed
  {
    .maxstack 8
    ldarg.0
    call instance void [mscorlib]System.Object::.ctor()
    ret
  }
}

.class public auto ansi beforefieldinit Driver
       extends [mscorlib]System.Object
{
  .method public static int32 Main () cil managed
  {
    .entrypoint
    .maxstack 8
    .locals init (class [mscorlib]System.Type t)

    // By name rather than ldtoken: a token lays the class out while Main is
    // compiled, and the test then passes without having asked anything.
    call class [mscorlib]System.Reflection.Assembly [mscorlib]System.Reflection.Assembly::GetExecutingAssembly()
    ldstr "HoldsAMissingType"
    callvirt instance class [mscorlib]System.Type [mscorlib]System.Reflection.Assembly::GetType(string)
    stloc.0

    ldloc.0
    callvirt instance bool [mscorlib]System.Type::get_IsClass()
    pop

    // The class is still whole: a recorded failure makes this throw instead.
    ldloc.0
    callvirt instance class [mscorlib]System.Reflection.FieldInfo[] [mscorlib]System.Type::GetFields()
    ldlen
    conv.i4
    ldc.i4.1
    beq.s FIELD_SURVIVED

    ldstr "GetFields () did not answer with the one declared field"
    call void [mscorlib]System.Console::WriteLine(string)
    ldc.i4.1
    ret

  FIELD_SURVIVED:
    ldc.i4.0
    ret
  }
}
