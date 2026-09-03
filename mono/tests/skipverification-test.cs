using System;
using System.Runtime.CompilerServices;
using SkipVerificationLibrary;

// Built twice from this one source. The WAIVED build is compiled -unsafe, which
// makes a C# compiler emit the RequestMinimum SkipVerification permission set on
// the assembly. That permission waives the accessibility rules for every site in
// it. The other build is compiled without -unsafe, and the same accesses have to
// be refused there.
class Program
{
	// One access per method, and none of them inlined. A refused field access
	// fails the whole method that holds it, so an access sitting in Main would
	// take Main down with it and never reach a catch.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PrivateField () { return Target.privateField; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PrivateMethod () { return Target.PrivateStaticMethod (); }

	// ldftn rather than a call, because each engine checks the two in different
	// places.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PrivateMethodPointer ()
	{
		Func<int> f = new Func<int> (Target.PrivateStaticMethod);
		return f ();
	}

	static int failures;

	// Past the tier-1 threshold, so the compiled engine checks the access as
	// well as the interpreter.
	const int Calls = 40;

	static void Allowed (string what, Func<int> access, int want)
	{
		try {
			for (int i = 0; i < Calls; i++) {
				int got = access ();
				if (got != want) {
					Console.WriteLine ("{0}: wrong value {1}, want {2}", what, got, want);
					failures++;
					return;
				}
			}
			Console.WriteLine ("{0}: OK", what);
		} catch (MemberAccessException e) {
			Console.WriteLine ("{0}: refused - {1}", what, e.GetType ().Name);
			failures++;
		}
	}

	static void Refused (string what, Func<int> access)
	{
		try {
			for (int i = 0; i < Calls; i++)
				access ();
			Console.WriteLine ("{0}: allowed, want refused", what);
			failures++;
		} catch (MemberAccessException) {
			Console.WriteLine ("{0}: refused, as it should be", what);
		}
	}

	static void Check (string what, Func<int> access, int want)
	{
#if WAIVED
		Allowed (what, access, want);
#else
		Refused (what, access);
#endif
	}

	static int Main ()
	{
		Check ("private field", PrivateField, 42);
		Check ("private method", PrivateMethod, 43);
		Check ("private method pointer", PrivateMethodPointer, 43);

		Console.WriteLine (failures == 0 ? "OK" : failures + " failures");
		return failures;
	}
}
