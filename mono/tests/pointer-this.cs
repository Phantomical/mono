using System;

public struct S {
	public int x;

	public int Get () { return x + 1; }
}

/*
 * An instance method whose receiver is a native int rather than a pointer.
 * Pointer arithmetic and ldind.i both leave one on the stack, and the call
 * still has to hand the callee a pointer.
 */
public class Test {
	unsafe public static int Main ()
	{
		S a;

		a.x = 41;

		S *p = &a;
		int i = 0;

		/* add: the receiver is an i64 the arithmetic produced. */
		if ((p + i)->Get () != 42)
			return 1;

		S **pp = &p;

		/* ldind.i: the receiver is an i64 loaded out of memory. */
		if ((*pp)->Get () != 42)
			return 2;

		return 0;
	}
}
