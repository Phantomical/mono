using System;

// --llvm-opt=-mono-tier0-classic=OpenCallee names Main () and
// OpenCalleeCall<T> and nothing else, so SharedBox<T>'s own methods are left
// to whatever tier reaches them. Main () has to be classic for the filter to
// answer for its callee at all: the interpreter reaches a callee itself
// without asking the backend.
//
// OpenCalleeCall<T> compiles as its class-shared body, so the call sites in it
// name SharedBox<T>'s methods with the caller's own still-open T as the
// class's type argument. That is the identity the callee is then compiled
// under, and each of the three below reads its own class's runtime generic
// context - a cast, a type handle and an array class - so a context filled
// against an open type argument answers wrongly rather than silently.
//
// SharedBox<T> deliberately shares no substring with the filter, so the
// callees reach the classic compiler by no other route.
public class SharedBox<T>
{
	public object Boxed;

	public T Unwrap ()
	{
		return (T) Boxed;
	}

	public string ElementName ()
	{
		return typeof (T).Name;
	}

	public T[] Row ()
	{
		return new T[3];
	}
}

public class Tier0ClassicOpenCalleeTest
{
	static long OpenCalleeCall<T> (object item)
	{
		SharedBox<T> box = new SharedBox<T> ();
		long ok = 0;

		box.Boxed = item;

		if ((object) box.Unwrap () == item)
			ok |= 1;
		if (box.ElementName () == typeof (T).Name)
			ok |= 2;

		T[] row = box.Row ();

		if (row.Length == 3 && row.GetType ().GetElementType () == typeof (T))
			ok |= 4;

		return ok;
	}

	static readonly string[] Names = {
		"a cast to the class's type argument",
		"the class's type argument as a type handle",
		"an array of the class's type argument",
	};

	public static int Main ()
	{
		int failures = 0;

		foreach (long got in new long[] {
			OpenCalleeCall<string> ("hi"),
			OpenCalleeCall<object> (new Version (1, 2)),
			OpenCalleeCall<Version> (new Version (3, 4)),
		}) {
			for (int i = 0; i < Names.Length; i++) {
				if ((got & (1L << i)) == 0) {
					Console.WriteLine ("FAIL: {0}", Names [i]);
					failures++;
				}
			}
		}

		if (failures != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
