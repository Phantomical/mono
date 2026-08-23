using System;
using System.Runtime.CompilerServices;

/*
 * A newarr is served by the collector's array allocator where the collector has
 * one, and by the runtime's array-new icall where it has none. SGen has one and
 * Boehm has none, so each collector runs this program against a different arm.
 *
 * The lengths below are the ones the two arms answer differently on their own.
 * emit_vector_alloc () (mono/llvm/method-to-llvm/arrays.cpp) holds the split:
 * above MONO_ARRAY_MAX_INDEX the allocator raises OutOfMemoryException and the
 * icall raises OverflowException. emit_newarr () tests the length in front of
 * both, so every arm must answer OverflowException here.
 */
class NewarrRefusal {
	// The array is returned, and the caller cannot fold this body in, so the
	// allocation stands whatever the tier does with it.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static byte[] Allocate (long length)
	{
		return new byte[length];
	}

	static Type Refusal (long length)
	{
		try {
			Allocate (length);
			return null;
		} catch (Exception e) {
			return e.GetType ();
		}
	}

	static int Main ()
	{
		if (Refusal (-1) != typeof (OverflowException))
			return 1;
		if (Refusal (-6000000000) != typeof (OverflowException))
			return 2;
		if (Refusal (5000000000) != typeof (OverflowException))
			return 3;

		// 2^32 truncates to a legal zero. A length that reaches the icall's
		// int32 count gets an empty array instead of an exception.
		if (Refusal (4294967296) != typeof (OverflowException))
			return 4;

		// Whichever arm ran must still allocate, and zero what it allocated.
		byte[] taken = Allocate (64);

		if (taken.Length != 64)
			return 5;

		foreach (byte b in taken)
			if (b != 0)
				return 6;

		return 0;
	}
}
