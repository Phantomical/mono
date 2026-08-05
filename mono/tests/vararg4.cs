using System;

//
// Values crossing the fixed/variable boundary of a vararg call.
//
// ArgIterator walks the variable part by mono_type_stack_size, which is not a
// whole word for every type - a float takes four bytes - so a caller that laid
// the buffer out by any other rule would still deliver the first argument and
// then hand back garbage for everything behind it. Hence the mixed types and
// the interleaved floats.
//

struct Pair {
	public int a;
	public long b;
}

class VarargValues
{
	static int Mixed (int first, string second, __arglist)
	{
		if (first != 11)
			return 1;
		if (second != "fixed")
			return 2;

		ArgIterator it = new ArgIterator (__arglist);

		if (it.GetRemainingCount () != 7)
			return 3;

		if ((int) TypedReference.ToObject (it.GetNextArg ()) != 22)
			return 4;
		if ((float) TypedReference.ToObject (it.GetNextArg ()) != 3.5f)
			return 5;
		if ((long) TypedReference.ToObject (it.GetNextArg ()) != 0x1234567890L)
			return 6;
		if ((float) TypedReference.ToObject (it.GetNextArg ()) != -0.25f)
			return 7;
		if ((double) TypedReference.ToObject (it.GetNextArg ()) != 6.75)
			return 8;
		if ((string) TypedReference.ToObject (it.GetNextArg ()) != "tail")
			return 9;

		Pair p = (Pair) TypedReference.ToObject (it.GetNextArg ());

		if (p.a != 42 || p.b != -7)
			return 10;
		if (it.GetRemainingCount () != 0)
			return 11;

		return 0;
	}

	// The types the iterator reports have to line up with the values too.
	static int Types (__arglist)
	{
		ArgIterator it = new ArgIterator (__arglist);

		if (Type.GetTypeFromHandle (it.GetNextArgType ()) != typeof (short))
			return 30;
		if ((short) TypedReference.ToObject (it.GetNextArg ()) != -300)
			return 31;
		if (Type.GetTypeFromHandle (it.GetNextArgType ()) != typeof (byte))
			return 32;
		if ((byte) TypedReference.ToObject (it.GetNextArg ()) != 200)
			return 33;
		if (Type.GetTypeFromHandle (it.GetNextArgType ()) != typeof (double))
			return 34;
		if ((double) TypedReference.ToObject (it.GetNextArg ()) != -1.5)
			return 35;

		return 0;
	}

	// A vararg method a caller passed nothing extra to still has to see an
	// empty variable part rather than whatever was left on the stack.
	static int Empty (int only, __arglist)
	{
		ArgIterator it = new ArgIterator (__arglist);

		if (it.GetRemainingCount () != 0)
			return 20;

		return only == 5 ? 0 : 21;
	}

	static int Main ()
	{
		Pair p;

		p.a = 42;
		p.b = -7;

		int r = Mixed (11, "fixed",
		               __arglist (22, 3.5f, 0x1234567890L, -0.25f, 6.75, "tail", p));

		if (r != 0) {
			Console.WriteLine ("Mixed failed: {0}", r);
			return r;
		}

		r = Types (__arglist ((short) -300, (byte) 200, -1.5));
		if (r != 0) {
			Console.WriteLine ("Types failed: {0}", r);
			return r;
		}

		r = Empty (5, __arglist ());
		if (r != 0) {
			Console.WriteLine ("Empty failed: {0}", r);
			return r;
		}

		return 0;
	}
}
