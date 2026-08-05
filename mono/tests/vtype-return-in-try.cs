using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

// A value type returned across the runtime's boundary convention does not come
// back in the natural way: it arrives through a pointer the caller supplies, or
// as register words, and either way the caller reads it back after the call.
// When the call sits inside a try, its result reaches the rest of the method
// along the invoke's normal edge - and if the value feeds a loop-carried local
// there is a phi waiting at the top of that edge. The read-back has to be
// sequenced ahead of the phi; put it after and the phi names a value that does
// not exist yet, and the loop carries whatever was lying around instead.
//
// This is the shape of every LINQ aggregate over decimal: Max/Min/Average all
// walk an enumerator inside a `using`, so the by-address return of get_Current
// lands on an invoke's normal edge and flows into the running best.
//
// Compiled with -optimize (special-tests.cmake): the phi only survives from
// optimized IL, and without it these methods pass either way.

struct Small {
	public int lo, hi;

	public Small (int v) { lo = v; hi = -v; }
}

interface ISource {
	bool MoveNext ();
	Small Current { get; }
}

class Source : ISource, IDisposable {
	int [] values;
	int at = -1;

	public Source (int [] v) { values = v; }

	public bool MoveNext () { return ++at < values.Length; }
	public Small Current { get { return new Small (values [at]); } }
	public void Dispose () { }
}

class Test {
	// 16 bytes: too wide for the register words, so it travels by address.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static decimal MaxDecimal (IEnumerable<decimal> source)
	{
		decimal best;

		using (IEnumerator<decimal> e = source.GetEnumerator ()) {
			if (!e.MoveNext ())
				throw new InvalidOperationException ();
			best = e.Current;
			while (e.MoveNext ()) {
				decimal x = e.Current;
				if (x > best)
					best = x;
			}
		}
		return best;
	}

	// 8 bytes: comes back in a register word.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static Small MaxSmall (ISource source)
	{
		Small best;

		using (source as IDisposable) {
			if (!source.MoveNext ())
				throw new InvalidOperationException ();
			best = source.Current;
			while (source.MoveNext ()) {
				Small x = source.Current;
				if (x.lo > best.lo)
					best = x;
			}
		}
		return best;
	}

	public static int Main ()
	{
		// The largest first, so carrying the wrong value shows up: the
		// broken form ends up holding the last element instead.
		if (MaxDecimal (new decimal [] { 5m, 2m, 3m, 4m }) != 5m)
			return 1;
		if (MaxDecimal (new decimal [] { -7.25m, 3m, -1000.5m, 2m }) != 3m)
			return 2;

		Small s = MaxSmall (new Source (new int [] { 5, 2, 3, 4 }));

		if (s.lo != 5 || s.hi != -5)
			return 3;

		return 0;
	}
}
