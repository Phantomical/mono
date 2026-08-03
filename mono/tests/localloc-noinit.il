/*
using System;

class A
{
	// In the il, remove "init" from the ".locals" of this function: a method
	// without the localsinit flag cannot be written in C# directly.
	//
	// Both blocks come from two calls to the *same* method on purpose. A given
	// method's localloc lands at the same stack offset on every call, so the
	// second call is handed exactly the bytes the first one left behind, and
	// "was it zeroed?" is the only thing left to observe. Comparing the blocks
	// of two *different* methods would instead depend on their frames coming
	// out byte-identical, which no optimizing code generator promises -- that
	// is what an earlier version of this test did, and it broke as soon as one
	// of the two loops vectorized and the other did not.
	static unsafe byte* Probe (int size, byte value, bool write)
	{
		byte* data = stackalloc byte [0x10000 + size];
		if (write)
			for (int i = 0; i < size; ++i)
				data [i] = value;
		return data;
	}

	static void assert (int a, int b, int c, bool f)
	{
		if (f)
			return;
		Console.WriteLine ("assert failed {0} {1} {2}", a, b, c);
		Environment.Exit (1);
	}

	unsafe public static void Main ()
	{
		int count = 123;
		byte value = 31;

		byte* first = Probe (count, value, true);
		byte* second = Probe (count, 0, false);

		// Same method, same call depth: the same memory both times.
		assert (1, 0, 0, first == second);

		// localsinit is off, so the second localloc left it alone.
		for (int i = 0; i < count; ++i)
			assert (2, i, second [i], second [i] == value);
	}
}
*/

.assembly extern mscorlib { }

.assembly '1' { }

.module '1.exe'

.class A
{
  .method static uint8* Probe (int32 size, uint8 valu, bool write) noinlining
  {
    .maxstack  3
    .locals      (uint8* data, int32 i)
//  .locals init (uint8* data, int32 i)
	ldc.i4     0x10000
	ldarg size
	add
	localloc
	stloc data

	ldarg write
	brfalse    IL_DONE

	ldc.i4 0
	stloc i
	br         IL_COND

IL_BODY:
	ldloc data
	ldloc i
	add
	ldarg valu
	stind.i1
	ldloc i
	ldc.i4 1
	add
	stloc i
IL_COND:
	ldloc i
	ldarg size
	clt
	brtrue     IL_BODY

IL_DONE:
	ldloc data
	ret
  }

  .method static void  'assert' (int32 a, int32 b, int32 c, bool f)
  {
	.maxstack  4

	ldarg.3
	brfalse.s  IL_0008
	br.s       IL_002c

IL_0008:
	ldstr      "assert failed {0} {1} {2}"
	ldarg.0
	box        [mscorlib]System.Int32
	ldarg 1
	box        [mscorlib]System.Int32
	ldarg 2
	box        [mscorlib]System.Int32
	call       void [mscorlib]System.Console::WriteLine (string, object, object, object)
	ldc.i4 1
	call       void [mscorlib]System.Environment::Exit (int32)
IL_002c:
	ret
  }

  .method public static void Main ()
  {
    .entrypoint
    .maxstack  5
    .locals init (int32 count, uint8 valu, uint8* first, uint8* second, int32 i)

	ldc.i4   123
	stloc count
	ldc.i4   31
	stloc valu

// --------------------------------------------------

	ldloc count
	ldloc valu
	ldc.i4.1
	call       uint8* A::Probe (int32, uint8, bool)
	stloc first

	ldloc count
	ldc.i4.0
	ldc.i4.0
	call       uint8* A::Probe (int32, uint8, bool)
	stloc second

// --------------------------------------------------

	ldc.i4 1
	ldc.i4 0
	ldc.i4 0
	ldloc first
	ldloc second
	ceq
	call       void A::'assert' (int32, int32, int32, bool)

// --------------------------------------------------

	ldc.i4.0
	stloc i
	br.s       IL_COND

IL_BODY:
	ldc.i4 2
	ldloc    i
	ldloc second
	ldloc    i
	add
	ldind.u1
	dup
	ldloc valu
	ceq
	call       void A::'assert' (int32, int32, int32, bool)
	ldloc    i
	ldc.i4 1
	add
	stloc    i
IL_COND:
	ldloc    i
	ldloc count
	clt
	brtrue   IL_BODY
	ret
  }
}
