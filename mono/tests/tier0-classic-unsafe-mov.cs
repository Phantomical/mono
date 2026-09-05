using System;
using System.Collections.Generic;

// EqualityComparer<T> answers with LongEnumEqualityComparer<T> for an enum over
// a 64-bit type, and that comparer reaches Array.UnsafeMov<S,R> to read the
// enum as its underlying type. UnsafeMov's own IL boxes S and unboxes it as R,
// which throws whenever the two types differ, so the JIT intrinsic that
// replaces the call is the implementation of the method rather than an
// optimization of it.
//
// The two enums below are over ulong and uint, so R differs from S in
// signedness in one case and in width as well in the other. Neither reaches
// this file's own methods, so the suite names the corlib caller
// (UnsafeEnumCast) rather than a name declared here.
public class Tier0ClassicUnsafeMovTest
{
	enum LongEnum : ulong { A = 1, B = 2 }
	enum IntEnum : uint { A = 3, B = 4 }

	public static int Main ()
	{
		int got = EqualityComparer<LongEnum>.Default.GetHashCode (LongEnum.B);
		int want = ((long) 2).GetHashCode ();

		if (got != want) {
			Console.WriteLine ("FAIL: LongEnum hash gave {0}, want {1}", got, want);
			return 1;
		}

		if (!EqualityComparer<LongEnum>.Default.Equals (LongEnum.A, LongEnum.A)
		    || EqualityComparer<LongEnum>.Default.Equals (LongEnum.A, LongEnum.B)) {
			Console.WriteLine ("FAIL: LongEnum equality");
			return 1;
		}

		got = EqualityComparer<IntEnum>.Default.GetHashCode (IntEnum.B);
		want = ((int) 4).GetHashCode ();

		if (got != want) {
			Console.WriteLine ("FAIL: IntEnum hash gave {0}, want {1}", got, want);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
