// The shapes of volatile. that C# has no spelling for (ECMA-335 III.2.6): the
// prefix on a value class, on an object reference, on a location wider than
// C# permits volatile on, and combined with unaligned. in both orders.
//
// Each of these takes a different route through the translator - only some of
// them can carry the ordering on the access itself - so this pins that every
// one of them still compiles and still round-trips its value.

.assembly extern mscorlib
{
  .ver 0:0:0:0
}
.assembly 'volatile-prefix-shapes'
{
  .hash algorithm 0x00008004
  .ver  0:0:0:0
}

.class public sequential ansi sealed beforefieldinit Pair
       extends [mscorlib]System.ValueType
{
  .field public int32 a
  .field public int32 b
}

// A value class with a reference in it goes through the collector's own copy
// routine rather than a memcpy.
.class public sequential ansi sealed beforefieldinit Tagged
       extends [mscorlib]System.ValueType
{
  .field public object tag
  .field public int32 n
}

.class public auto ansi beforefieldinit Test
       extends [mscorlib]System.Object
{
  .field public static int32 counter
  .field public static int64 wide
  .field public static float64 fp
  .field public static object slot
  .field public static valuetype Pair pair
  .field public static valuetype Tagged tagged

  .method public static int32 Main() cil managed
  {
    .entrypoint
    .maxstack 8
    .locals init (
      valuetype Pair v_pair,
      valuetype Tagged v_tagged,
      native int base_,
      native int odd,
      object o)

    // 1. int32 static: a scalar the access itself can carry the ordering on.
    ldc.i4 0x11223344
    volatile.
    stsfld int32 Test::counter
    volatile.
    ldsfld int32 Test::counter
    ldc.i4 0x11223344
    ceq
    brtrue ok1
    ldc.i4.1
    ret
  ok1:

    // 2. int64 and float64: wider than C# allows volatile on, but III.2.6 does.
    ldc.i8 0x0123456789abcdef
    volatile.
    stsfld int64 Test::wide
    volatile.
    ldsfld int64 Test::wide
    ldc.i8 0x0123456789abcdef
    ceq
    brtrue ok2
    ldc.i4.2
    ret
  ok2:

    ldc.r8 3.5
    volatile.
    stsfld float64 Test::fp
    volatile.
    ldsfld float64 Test::fp
    ldc.r8 3.5
    ceq
    brtrue ok3
    ldc.i4.3
    ret
  ok3:

    // 3. An object reference: the store goes through the write barrier, which
    //    does the store inside the call.
    newobj instance void [mscorlib]System.Object::.ctor()
    stloc.s o
    ldloc.s o
    volatile.
    stsfld object Test::slot
    volatile.
    ldsfld object Test::slot
    ldloc.s o
    ceq
    brtrue ok4
    ldc.i4.4
    ret
  ok4:

    // 4. A value class: a copy rather than one access.
    ldloca.s v_pair
    ldc.i4 7
    stfld int32 Pair::a
    ldloca.s v_pair
    ldc.i4 9
    stfld int32 Pair::b

    ldsflda valuetype Pair Test::pair
    ldloc.s v_pair
    volatile.
    stobj valuetype Pair

    ldsflda valuetype Pair Test::pair
    volatile.
    ldobj valuetype Pair
    stloc.s v_pair

    ldloca.s v_pair
    ldfld int32 Pair::a
    ldc.i4 7
    ceq
    brfalse fail5
    ldloca.s v_pair
    ldfld int32 Pair::b
    ldc.i4 9
    ceq
    brtrue ok5
  fail5:
    ldc.i4.5
    ret
  ok5:

    // 5. A value class the collector has to walk.
    ldloca.s v_tagged
    ldloc.s o
    stfld object Tagged::tag
    ldloca.s v_tagged
    ldc.i4 42
    stfld int32 Tagged::n

    ldsflda valuetype Tagged Test::tagged
    ldloc.s v_tagged
    volatile.
    stobj valuetype Tagged

    ldsflda valuetype Tagged Test::tagged
    volatile.
    ldobj valuetype Tagged
    stloc.s v_tagged

    ldloca.s v_tagged
    ldfld object Tagged::tag
    ldloc.s o
    ceq
    brfalse fail6
    ldloca.s v_tagged
    ldfld int32 Tagged::n
    ldc.i4 42
    ceq
    brtrue ok6
  fail6:
    ldc.i4.6
    ret
  ok6:

    // 6. unaligned. with volatile., in both orders. An access that cannot
    //    promise its own alignment cannot be an atomic one.
    ldc.i4 64
    localloc
    stloc.s base_
    ldloc.s base_
    ldc.i4.1
    add
    stloc.s odd

    ldloc.s odd
    ldc.i4 0x55667788
    unaligned. 1
    volatile.
    stind.i4

    ldloc.s odd
    volatile.
    unaligned. 1
    ldind.i4
    ldc.i4 0x55667788
    ceq
    brtrue ok7
    ldc.i4.7
    ret
  ok7:

    // 7. The same pair without the alignment caveat, so both halves of the
    //    fallback are covered by one shape.
    ldloc.s base_
    ldc.i4 0x778899aa
    volatile.
    stind.i4

    ldloc.s base_
    volatile.
    ldind.i4
    ldc.i4 0x778899aa
    ceq
    brtrue ok8
    ldc.i4.8
    ret
  ok8:

    ldc.i4.0
    ret
  }
}
