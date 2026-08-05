using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

//
// A value type carries bytes that no field covers: everything past the first
// element of a C# `fixed` buffer, the arms of an explicit layout that another
// field overlaps, and whatever an explicit Size leaves at the end. Those bytes
// are as live as any field, so a copy of the value has to carry them through.
//
class FixedBufferCopy {

	[StructLayout (LayoutKind.Explicit, Size = 12)]
	unsafe struct FixedInts {
		[FieldOffset (0)]
		public fixed int array [3];
	}

	unsafe struct FixedBytes {
		public fixed byte data [8];
	}

	[StructLayout (LayoutKind.Explicit, Size = 8)]
	struct Union {
		[FieldOffset (0)]
		public int first;
		[FieldOffset (0)]
		public long whole;
	}

	[StructLayout (LayoutKind.Sequential, Pack = 1, Size = 8)]
	struct SizedTail {
		public int head;
	}

	/* Through a call, so the copy is one the optimizer cannot see around. */
	[MethodImpl (MethodImplOptions.NoInlining)]
	static T Roundtrip<T> (T value)
	{
		T copy = value;

		return copy;
	}

	static unsafe int TestFixedInts ()
	{
		FixedInts s = new FixedInts ();

		s.array [0] = 1;
		s.array [1] = 2;
		s.array [2] = 3;

		FixedInts copy = s;

		if (copy.array [0] != 1 || copy.array [1] != 2 || copy.array [2] != 3) {
			Console.WriteLine ("fixed int copy: {0} {1} {2}", copy.array [0],
					   copy.array [1], copy.array [2]);
			return 1;
		}

		FixedInts passed = Roundtrip (s);

		if (passed.array [0] != 1 || passed.array [1] != 2 || passed.array [2] != 3) {
			Console.WriteLine ("fixed int call: {0} {1} {2}", passed.array [0],
					   passed.array [1], passed.array [2]);
			return 2;
		}

		return 0;
	}

	static unsafe int TestFixedBytes ()
	{
		FixedBytes s = new FixedBytes ();

		for (int i = 0; i < 8; ++i)
			s.data [i] = (byte) (i + 1);

		FixedBytes copy = Roundtrip (s);

		for (int i = 0; i < 8; ++i)
			if (copy.data [i] != (byte) (i + 1)) {
				Console.WriteLine ("fixed byte {0}: {1}", i, copy.data [i]);
				return 3;
			}

		return 0;
	}

	static int TestUnion ()
	{
		Union u = new Union ();

		u.whole = 0x0102030405060708L;

		Union copy = Roundtrip (u);

		if (copy.whole != 0x0102030405060708L) {
			Console.WriteLine ("union: {0:x}", copy.whole);
			return 4;
		}

		return 0;
	}

	static unsafe int TestSizedTail ()
	{
		SizedTail s = new SizedTail ();
		byte *bytes = (byte *) &s;

		s.head = 0x11223344;
		for (int i = 4; i < 8; ++i)
			bytes [i] = (byte) (i + 1);

		SizedTail copy = Roundtrip (s);
		byte *copied = (byte *) &copy;

		if (copy.head != 0x11223344) {
			Console.WriteLine ("sized tail head: {0:x}", copy.head);
			return 5;
		}

		for (int i = 4; i < 8; ++i)
			if (copied [i] != (byte) (i + 1)) {
				Console.WriteLine ("sized tail {0}: {1}", i, copied [i]);
				return 6;
			}

		return 0;
	}

	static int Main ()
	{
		int result;

		if ((result = TestFixedInts ()) != 0)
			return result;
		if ((result = TestFixedBytes ()) != 0)
			return result;
		if ((result = TestUnion ()) != 0)
			return result;
		if ((result = TestSizedTail ()) != 0)
			return result;

		return 0;
	}
}
