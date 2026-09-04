using System;
using System.Reflection;

// --llvm-opt=-mono-tier0-classic=GsharedShare filters three methods to the
// classic compiler: GsharedShareEnter, GsharedShareCaller<T> and the callee it
// calls using its own still-unresolved T, GsharedShareCallee<T>.
// GsharedShareCaller<T> and GsharedShareCallee<T> are both reference-type-
// sharable (T : class), so classic tier0 compiles each as its own shared body
// - one compile per method, reused for every reference-type T - rather than
// once per instantiation.
//
// This is the shape that crashed classic tier0's "compile everything" mode on
// System.Runtime.CompilerServices.Unsafe:AsPointer<T> (ref T): a shared
// caller calling another reference-sharable generic method by naming the
// caller's own still-open T as the callee's type argument. The caller's call
// site resolves cleanly to the callee's own shared thunk (no rgctx needed -
// two reference-shared bodies agree on the same "one pointer" representation
// already), but the callee's own compile, requested later when that thunk is
// first invoked, was never told it is itself a shared body and aborted on an
// assert that its signature still names an open type parameter.
// mono_tier0_compile ()'s JIT_FLAG_METHOD_IS_GSHARED fix is what this
// exercises.
//
// GsharedShareEnter is deliberately not generic. Entering a reference-shared
// generic method - one whose own signature still names an open type
// parameter - as the very first, reflection-invoked call hits a separate,
// unfixed crash in classic tier0's own runtime-generic-context plumbing, so
// GsharedShareEnter enters through reflection cleanly instead, the same way
// tier0-classic.cs's own Tier0ClassicExercise does, and calls
// GsharedShareCaller<T> with three concrete, closed T's by ordinary direct
// call - the same shape CultureData.get_Invariant used calling
// Interlocked.CompareExchange<CultureData>.
public class Tier0ClassicGsharedVtTest
{
	static string GsharedShareCallee<T> (T value) where T : class
	{
		return value == null ? "null" : value.GetType ().Name;
	}

	static string GsharedShareCaller<T> (T value) where T : class
	{
		return GsharedShareCallee<T> (value);
	}

	sealed class Widget { }

	static long[] GsharedShareEnter ()
	{
		return new long[] {
			GsharedShareCaller<string> ("hi") == "String" ? 1 : 0,
			GsharedShareCaller<object> (new Widget ()) == "Widget" ? 1 : 0,
			GsharedShareCaller<Widget> (null) == "null" ? 1 : 0,
		};
	}

	static readonly string[] Names = {
		"string T", "object T / Widget instance", "Widget T / null",
	};

	static int Check (long[] r)
	{
		int failures = 0;

		for (int i = 0; i < r.Length; i++) {
			if (r [i] != 1) {
				Console.WriteLine ("FAIL: {0}", Names [i]);
				failures++;
			}
		}

		return failures;
	}

	public static int Main ()
	{
		// A direct call from Main, itself interpreted, never asks the backend
		// for its callee - the interpreter resolves and runs one on its own,
		// same as tier0-classic.cs's own Main. Reflection crosses through a
		// compiled runtime-invoke wrapper instead, which is what gives
		// GsharedShareEnter's own entry a chance to answer the filter.
		MethodInfo enter = typeof (Tier0ClassicGsharedVtTest).GetMethod (
			"GsharedShareEnter", BindingFlags.Static | BindingFlags.NonPublic);

		int failures = Check ((long[]) enter.Invoke (null, null));

		if (failures != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
