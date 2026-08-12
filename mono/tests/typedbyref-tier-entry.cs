using System;

// A compiled caller entering an interpreted method whose signature carries a
// TypedReference. The engine boundary has to convert that argument, and a call
// between two interpreted methods never converts, so which methods are
// interpreted is pinned rather than left to promotion.
class TypedByRefTierEntry
{
	static int InterpMeTake (TypedReference tr)
	{
		return __refvalue (tr, int);
	}

	static string InterpMeDescribe (TypedReference tr)
	{
		return __reftype (tr).Name;
	}

	public static int Main ()
	{
		int value = 42;

		int got = InterpMeTake (__makeref (value));
		if (got != 42) {
			Console.WriteLine ("read back {0}", got);
			return 1;
		}

		string name = InterpMeDescribe (__makeref (value));
		if (name != "Int32") {
			Console.WriteLine ("type is {0}", name);
			return 1;
		}

		Console.WriteLine ("ok");
		return 0;
	}
}
