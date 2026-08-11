/*
using System;

class A
{
	// In the il, remove "init" from the ".locals" of this function: a method
	// without the localsinit flag cannot be written in C# directly. That flag is
	// the whole subject -- without it a localloc hands back whatever the memory
	// already held, and this checks that the runtime really does leave it alone.
	//
	// Both calls go to the *same* method on purpose. A given method's localloc
	// lands at the same offset on every call, so the second call is handed
	// exactly the bytes the first one left behind, and "was it zeroed?" is the
	// only thing left to observe.
	//
	// The check runs inside the second call rather than back in Main, because
	// anything invoked in between gets a frame over the very memory under test
	// -- the block is below the stack pointer once Probe returns, so the next
	// call writes through it. That is also why the block is small: it does not
	// need to outrun an intervening frame, and the interpreter has its own
	// reason to want it small (below).
	static unsafe bool Probe (int size, byte value, bool write)
	{
		byte* data = stackalloc byte [0x400 + size];

		if (write) {
			for (int i = 0; i < size; ++i)
				data [i] = value;
			return true;
		}

		for (int i = 0; i < size; ++i)
			if (data [i] != value)
				return false;
		return true;
	}

	unsafe public static void Main ()
	{
		Probe (123, 31, true);
		if (!Probe (123, 31, false)) {
			Console.WriteLine ("localloc block was cleared or overwritten");
			Environment.Exit (1);
		}
	}
}

Keep the allocation inside the interpreter's frame-data fragment, which starts
at 8192 bytes. A request bigger than that leaves the fragment and becomes a
g_free/g_malloc round trip instead, and while glibc does hand the same chunk
back, it writes its own free-list bookkeeping into the head of it -- so the
first bytes come back as a pointer rather than as what was stored there.
*/

.assembly extern mscorlib { }

.assembly '1' { }

.module '1.exe'

.class A
{
  .method static bool Probe (int32 size, uint8 valu, bool write) noinlining
  {
    .maxstack  3
    .locals      (uint8* data, int32 i)
//  .locals init (uint8* data, int32 i)
	ldc.i4     0x400
	ldarg size
	add
	localloc
	stloc data

	ldc.i4 0
	stloc i
	ldarg write
	brfalse    IL_CHECK
	br         IL_WCOND

IL_WBODY:
	ldloc data
	ldloc i
	add
	ldarg valu
	stind.i1
	ldloc i
	ldc.i4 1
	add
	stloc i
IL_WCOND:
	ldloc i
	ldarg size
	clt
	brtrue     IL_WBODY
	ldc.i4.1
	ret

IL_CHECK:
	br         IL_CCOND
IL_CBODY:
	ldloc data
	ldloc i
	add
	ldind.u1
	ldarg valu
	beq        IL_CNEXT
	ldc.i4.0
	ret
IL_CNEXT:
	ldloc i
	ldc.i4 1
	add
	stloc i
IL_CCOND:
	ldloc i
	ldarg size
	clt
	brtrue     IL_CBODY
	ldc.i4.1
	ret
  }

  .method public static void Main ()
  {
    .entrypoint
    .maxstack  4
    .locals init (int32 count, uint8 valu)

	ldc.i4   123
	stloc count
	ldc.i4   31
	stloc valu

	ldloc count
	ldloc valu
	ldc.i4.1
	call       bool A::Probe (int32, uint8, bool)
	pop

	ldloc count
	ldloc valu
	ldc.i4.0
	call       bool A::Probe (int32, uint8, bool)
	brtrue     IL_OK

	ldstr      "localloc block was cleared or overwritten"
	call       void [mscorlib]System.Console::WriteLine (string)
	ldc.i4 1
	call       void [mscorlib]System.Environment::Exit (int32)
IL_OK:
	ret
  }
}
