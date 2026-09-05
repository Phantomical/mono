using System;
using System.Runtime.CompilerServices;

// --llvm-opt=-mono-tier0-classic=ClassInit puts the three entries below on the
// classic compiler. Each is the first thing its class is reached through, and
// each has to find the class initialized: tier 0 is where a class initializer
// runs, and a call to a static method carries no check of its own.
//
// ClassInitPlain covers a static method, ClassInitAggressive one the callee
// itself is marked AggressiveInlining on, and ClassInitField a static field
// read. Each cctor writes a flag on this class, so a run that skipped one
// reads as a wrong answer rather than a crash.
public class Tier0ClassicClassInitTest
{
	static int plainRan, aggressiveRan, fieldRan;

	class ClassInitPlain {
		static ClassInitPlain () { plainRan = 1; }
		public static int Answer () { return 11; }
	}

	class ClassInitAggressive {
		static ClassInitAggressive () { aggressiveRan = 1; }

		[MethodImpl (MethodImplOptions.AggressiveInlining)]
		public static int Answer () { return 22; }
	}

	class ClassInitField {
		static ClassInitField () { fieldRan = 1; }
		public static int Value = 33;
	}

	public static int Main ()
	{
		if (ClassInitPlain.Answer () != 11 || plainRan != 1) {
			Console.WriteLine ("FAIL: a static call did not run the class initializer");
			return 1;
		}

		if (ClassInitAggressive.Answer () != 22 || aggressiveRan != 1) {
			Console.WriteLine ("FAIL: an AggressiveInlining call did not run the class initializer");
			return 1;
		}

		if (ClassInitField.Value != 33 || fieldRan != 1) {
			Console.WriteLine ("FAIL: a static field read did not run the class initializer");
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
