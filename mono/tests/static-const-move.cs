// Checks that a readonly static holding an object reference still answers with that
// object after the collector has moved it.
//
// Each case promotes its reader while the object is still in the nursery, collects,
// and compares what the compiled body answers against a read from Main, which stays
// at tier 0 and folds nothing. The cases whose static holds no reference are here so
// that a fix which stops folding altogether fails something.
//
// SGen only: the bug needs a collector that moves.

using System;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

// One reference in one word, which is ImmutableArray<T>'s shape and the one the
// Roslyn heap corruption was found on: the field's declared type is not a
// reference, so a guard that reads the declared type alone lets it through.
struct OneWord {
	public readonly object Value;

	public OneWord (object value) { Value = value; }
}

// The same with the reference at a non-zero offset.
struct TwoWords {
	public readonly long Tag;
	public readonly object Value;

	public TwoWords (long tag, object value) { Tag = tag; Value = value; }
}

struct NoRefs {
	public readonly int Low;
	public readonly int High;

	public NoRefs (int low, int high) { Low = low; High = high; }
}

class Box {
	public OneWord Field;
}

static class Held {
	public static readonly OneWord Wrapped = new OneWord (Fresh ());
	public static readonly TwoWords Tagged = new TwoWords (7, Fresh ());
	public static readonly object Bare = Fresh ();
	public static readonly NoRefs Scalars = new NoRefs (11, 22);
	public static readonly int Number = 33;

	// Not inlined, so the only lasting copy of the new object's address is the
	// static field. A copy left in Main's frame would be pinned by the
	// conservative stack scan and the object would not move.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static object Fresh ()
	{
		return new int [3];
	}
}

class StaticConstMove {
	// MonoTier::tier1 and MonoTier::tier2, as PromoteNow takes them.
	const int tier1 = 3;
	const int tier2 = 4;

	static int fails;
	static object sink;

	static void Fail (string message)
	{
		Console.WriteLine ("FAIL: {0}", message);
		++fails;
	}

	// A body that only copies the struct reaches the fold; one that reads the
	// reference back out gets a pointer, which the pass has nothing to fold. The
	// corrupted methods were all of the copying kind -- a return, a field store,
	// an argument passed on -- so the dereferencing readers below are a negative
	// control, not a second case that matters.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static OneWord ReturnWrapped () { return Held.Wrapped; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Box StoreWrapped () { Box b = new Box (); b.Field = Held.Wrapped; return b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Box PassWrapped () { return Take (Held.Wrapped); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Box Take (OneWord w) { Box b = new Box (); b.Field = w; return b; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static TwoWords ReturnTagged () { return Held.Tagged; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object ReadWrappedField () { return Held.Wrapped.Value; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object ReadWrappedCopy () { OneWord w = Held.Wrapped; return w.Value; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object ReadTaggedField () { return Held.Tagged.Value; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object ReadTaggedCopy () { TwoWords t = Held.Tagged; return t.Value; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static object ReadBare () { return Held.Bare; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadScalars () { NoRefs n = Held.Scalars; return n.Low + n.High; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadNumber () { return Held.Number; }

	static MethodInfo MethodOf (string name)
	{
		return typeof (StaticConstMove).GetMethod (name,
			BindingFlags.Static | BindingFlags.NonPublic);
	}

	static void Promote (string name, int tier)
	{
		if (!Mono.Tiering.MonoTier.PromoteNow (MethodOf (name).MethodHandle.Value, tier))
			Fail (String.Format ("{0} would not compile at tier {1}", name, tier));
	}

	// Reads a scalar rather than one of the object-bearing statics, so running the
	// type initializer leaves no reference in this frame for the stack scan to pin.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Warm ()
	{
		return Held.Number;
	}

	// Overwrites the frames the calls above left behind. A dead slot still holding
	// one of the objects pins it, and a pinned object does not move.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static void Churn ()
	{
		object [] junk = new object [256];

		for (int i = 0; i < junk.Length; i++)
			junk [i] = new byte [64];

		sink = junk;
		sink = null;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenerationOfBare ()
	{
		return GC.GetGeneration (Held.Bare);
	}

	// ReferenceEquals is the whole check, and it has to stay the whole check: a
	// compiled body that answered with the pre-move address is holding memory the
	// collector has given to something else, and anything that reads through it --
	// printing it, hashing it, asking for its type -- corrupts the heap instead of
	// reporting. So the message names the case and nothing about the values.
	static void SameObject (string what, object compiled, object direct)
	{
		if (!Object.ReferenceEquals (compiled, direct))
			Fail (String.Format ("{0}: the compiled body answered an address the "
					+ "collector has moved away from", what));
	}

	public static int Main ()
	{
		int tier = Environment.GetEnvironmentVariable ("MONO_STATIC_CONST_TIER") == "1"
				? tier1 : tier2;

		Warm ();

		if (GenerationOfBare () != 0) {
			// The fold has to happen while the address is still the nursery one,
			// so a collection between the type initializer and the promotion
			// below leaves nothing for the rest of this to catch.
			Console.WriteLine ("FAIL: the statics left the nursery before the readers were compiled");
			return 1;
		}

		Promote ("ReturnWrapped", tier);
		Promote ("StoreWrapped", tier);
		Promote ("PassWrapped", tier);
		Promote ("ReturnTagged", tier);
		Promote ("ReadWrappedField", tier);
		Promote ("ReadWrappedCopy", tier);
		Promote ("ReadTaggedField", tier);
		Promote ("ReadTaggedCopy", tier);
		Promote ("ReadBare", tier);
		Promote ("ReadScalars", tier);
		Promote ("ReadNumber", tier);

		Churn ();
		GC.Collect ();
		GC.WaitForPendingFinalizers ();
		GC.Collect ();

		if (GenerationOfBare () == 0) {
			Console.WriteLine ("FAIL: the statics did not leave the nursery, so nothing moved");
			return 1;
		}

		SameObject ("OneWord returned by value", ReturnWrapped ().Value, Held.Wrapped.Value);
		SameObject ("OneWord stored into a field", StoreWrapped ().Field.Value, Held.Wrapped.Value);
		SameObject ("OneWord passed on as an argument", PassWrapped ().Field.Value, Held.Wrapped.Value);
		SameObject ("TwoWords returned by value", ReturnTagged ().Value, Held.Tagged.Value);
		SameObject ("OneWord.Value through ldsflda", ReadWrappedField (), Held.Wrapped.Value);
		SameObject ("OneWord.Value through ldsfld", ReadWrappedCopy (), Held.Wrapped.Value);
		SameObject ("TwoWords.Value through ldsflda", ReadTaggedField (), Held.Tagged.Value);
		SameObject ("TwoWords.Value through ldsfld", ReadTaggedCopy (), Held.Tagged.Value);
		SameObject ("a plain reference static", ReadBare (), Held.Bare);

		if (ReadScalars () != Held.Scalars.Low + Held.Scalars.High)
			Fail ("a struct static with no references answered wrongly");

		if (ReadNumber () != Held.Number)
			Fail ("a scalar static answered wrongly");

		// No collection after this point. StoreWrapped () and PassWrapped () each
		// leave a live object holding whatever they were given, and collecting
		// with a stale address among them corrupts the heap instead of letting
		// the count below be printed.
		Console.WriteLine (fails == 0 ? "OK" : String.Format ("{0} failure(s)", fails));
		return fails == 0 ? 0 : 1;
	}
}
