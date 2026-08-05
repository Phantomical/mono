using System;
using System.Runtime.CompilerServices;

//
// A constrained. callvirt whose receiver does not override the method boxes it, and
// boxing a Nullable<T> yields a boxed T - or null when it has no value - never a boxed
// Nullable<T>. Object::GetType () is the one place a program can see which of the two
// it got.
//

class Test {
	[MethodImpl (MethodImplOptions.NoInlining)]
	static string NameOfType<T> (T value)
	{
		return value.GetType ().ToString ();
	}

	static int Main ()
	{
		int? some = 4;

		if (some.GetType () != typeof (int))
			return 1;
		if (NameOfType (some) != "System.Int32")
			return 2;

		double? d = 1.5;
		if (d.GetType () != typeof (double))
			return 3;

		DayOfWeek? e = DayOfWeek.Friday;
		if (e.GetType () != typeof (DayOfWeek))
			return 4;

		/* A receiver with no value boxes to null, so the call throws. */
		int? none = null;
		try {
			none.GetType ();
			return 5;
		} catch (NullReferenceException) {
		}

		/* Nullable<T> overrides these, so the receiver is taken directly. */
		if (none.ToString () != "")
			return 6;
		if (some.ToString () != "4")
			return 7;

		/* The box opcode's own path, for the same rule. */
		object boxed = (object) some;
		if (boxed.GetType () != typeof (int))
			return 8;
		if ((object) none != null)
			return 9;

		return 0;
	}
}
