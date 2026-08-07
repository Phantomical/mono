using System;

// Class-initialization checks around exception handling.
//
// A check inside a try has not necessarily run when the try's handler runs, so
// it does not cover an access the handler can reach. Probe () below is the case
// that catches getting this wrong: with blowUp set, the try throws before the
// access inside it, and the access after the catch is the one that has to
// initialize the class.

class Log {
	public static int LazyRuns;
	public static int OtherRuns;
	public static int GuardedRuns;
	public static int FragileRuns;
}

class Fragile {
	public static int Value = 5;

	static Fragile ()
	{
		Log.FragileRuns++;
		throw new InvalidOperationException ("fragile");
	}
}

class Lazy {
	public static int Value;

	static Lazy ()
	{
		Log.LazyRuns++;
		Value = 42;
	}
}

class Other {
	public static int Touched;

	static Other ()
	{
		Log.OtherRuns++;
		Touched = 100;
	}
}

class Guarded {
	public static int Value;

	static Guarded ()
	{
		Log.GuardedRuns++;
		Value = 7;
	}
}

class CctorInitEh {
	static int Probe (bool blowUp)
	{
		int seen = -1;

		try {
			if (blowUp)
				throw new InvalidOperationException ();
			seen = Lazy.Value;
		} catch (InvalidOperationException) {
			Other.Touched++;
		}

		return seen + Lazy.Value;
	}

	// A finally is on the way out of the try however the try ends, so a class
	// it touches gets its own check there.
	static int Finally (bool blowUp)
	{
		int seen = 0;

		try {
			if (blowUp)
				throw new InvalidOperationException ();
			seen = Guarded.Value;
		} catch (InvalidOperationException) {
		} finally {
			seen += Guarded.Value;
		}

		return seen;
	}

	// Accesses after a check in the same try are covered by it; the ones the
	// handler reaches are not.
	static int Nested ()
	{
		int total = 0;

		try {
			total += Lazy.Value;
			total += Lazy.Value;
			throw new InvalidOperationException ();
		} catch (InvalidOperationException) {
			total += Lazy.Value;
		}

		return total + Lazy.Value;
	}

	// The case that tells dominance over the whole block apart from dominance
	// over the check's normal edge. The check inside the try is the thing that
	// throws, so the catch reaches everything after it with the class still
	// uninitialized - and every one of those accesses has to check again.
	static int AfterFailedInit ()
	{
		int caught = 0;
		int seen = -1;

		try {
			seen = Fragile.Value;
		} catch (TypeInitializationException) {
			caught++;
			try {
				seen = Fragile.Value;
			} catch (TypeInitializationException) {
				caught++;
			}
		}

		if (caught != 2 || seen != -1)
			return -1;

		// Not covered by the check inside the try either: this has to throw
		// rather than read a static of a class that never initialized.
		return Fragile.Value;
	}

	static bool ThrowsTypeInit (Func<int> body)
	{
		try {
			Console.WriteLine ("unexpectedly read {0}", body ());
			return false;
		} catch (TypeInitializationException) {
			return true;
		}
	}

	static int Main ()
	{
		// First touch of Lazy has to come from the access after the catch.
		int blown = Probe (true);

		if (blown != 41) {
			Console.WriteLine ("Probe (true) = {0}, expected 41", blown);
			return 1;
		}

		if (Log.LazyRuns != 1) {
			Console.WriteLine ("Lazy's cctor ran {0} times, expected 1",
			                   Log.LazyRuns);
			return 2;
		}

		if (Log.OtherRuns != 1 || Other.Touched != 101) {
			Console.WriteLine ("Other: runs {0}, Touched {1}; expected 1 and 101",
			                   Log.OtherRuns, Other.Touched);
			return 3;
		}

		int quiet = Probe (false);

		if (quiet != 84) {
			Console.WriteLine ("Probe (false) = {0}, expected 84", quiet);
			return 4;
		}

		// Same again for the finally path, where Guarded is still untouched.
		int guarded = Finally (true);

		if (guarded != 7 || Log.GuardedRuns != 1) {
			Console.WriteLine ("Finally (true) = {0} after {1} cctor runs; "
			                   + "expected 7 and 1", guarded, Log.GuardedRuns);
			return 5;
		}

		if (Finally (false) != 14) {
			Console.WriteLine ("Finally (false) = {0}, expected 14", Finally (false));
			return 6;
		}

		if (Nested () != 168) {
			Console.WriteLine ("Nested () = {0}, expected 168", Nested ());
			return 7;
		}

		if (Log.LazyRuns != 1 || Log.GuardedRuns != 1) {
			Console.WriteLine ("cctors re-ran: Lazy {0}, Guarded {1}",
			                   Log.LazyRuns, Log.GuardedRuns);
			return 8;
		}

		if (!ThrowsTypeInit (AfterFailedInit)) {
			Console.WriteLine ("an access the catch reached did not check");
			return 9;
		}

		if (Log.FragileRuns != 1) {
			Console.WriteLine ("Fragile's cctor ran {0} times, expected 1",
			                   Log.FragileRuns);
			return 10;
		}

		return 0;
	}
}
