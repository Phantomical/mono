using System;
using System.Runtime.InteropServices;
using System.Threading;

/* Unsafe is implemented by JIT intrinsics rather than executable IL. This
 * test covers its direct users as well as the related Interlocked and
 * Volatile paths when tier 0 is disabled. */
public class UnsafeIntrinsicsTest
{
	static int failures;

	static void Check (string what, bool ok)
	{
		if (ok)
			return;

		Console.WriteLine ("FAIL: {0}", what);
		++failures;
	}

	public static int Main ()
	{
		StringIndexerTest ();
		VolatileTest ();
		GCHandleTest ();
		InterlockedGenericTest ();

		if (failures != 0) {
			Console.WriteLine ("{0} failures", failures);
			return 1;
		}

		Console.WriteLine ("done!");
		return 0;
	}

	// String's own indexer is `Unsafe.Add (ref m_firstChar, index)`
	// (ReferenceSources/String.cs).
	static void StringIndexerTest ()
	{
		string s = "Kerbal";

		for (int i = 0; i < s.Length; ++i)
			Check ("string indexer [" + i + "]", s[i] == "Kerbal"[i]);
	}

	// Every Volatile.Read/Write overload but long, ulong and double is
	// `Unsafe.As<T, VolatileT> (ref location).Value` over a wrapper struct
	// with a genuinely volatile field (System.Threading/Volatile.cs); those
	// three are icalls of their own (method-to-llvm/volatile.cpp).
	class Boxed
	{
		public int Value = 11;
	}

	static void VolatileTest ()
	{
		bool vb = false;
		Volatile.Write (ref vb, true);
		Check ("Volatile bool", Volatile.Read (ref vb));

		byte vy = 0;
		Volatile.Write (ref vy, (byte) 0xAB);
		Check ("Volatile byte", Volatile.Read (ref vy) == 0xAB);

		short vs = 0;
		Volatile.Write (ref vs, (short) -1234);
		Check ("Volatile short", Volatile.Read (ref vs) == -1234);

		int vi = 0;
		Volatile.Write (ref vi, 0x11223344);
		Check ("Volatile int", Volatile.Read (ref vi) == 0x11223344);

		long vl = 0;
		Volatile.Write (ref vl, 0x1122334455667788L);
		Check ("Volatile long", Volatile.Read (ref vl) == 0x1122334455667788L);

		double vd = 0;
		Volatile.Write (ref vd, 3.5);
		Check ("Volatile double", Volatile.Read (ref vd) == 3.5);

		object vo = null;
		Boxed boxed = new Boxed ();
		Volatile.Write (ref vo, boxed);
		Check ("Volatile object", ReferenceEquals (Volatile.Read (ref vo), boxed));
	}

	// GCHandle.Target reads and writes a strong handle with `Unsafe.As<IntPtr,
	// object>` (System.Runtime.InteropServices/GCHandle.cs).
	static void GCHandleTest ()
	{
		Boxed value = new Boxed ();
		GCHandle handle = GCHandle.Alloc (value);

		try {
			Check ("GCHandle.Target read", ReferenceEquals (handle.Target, value));

			Boxed other = new Boxed ();
			handle.Target = other;
			Check ("GCHandle.Target write", ReferenceEquals (handle.Target, other));
		} finally {
			handle.Free ();
		}
	}

	// `T CompareExchange<T> (ref T, T, T) where T : class` and `T
	// Exchange<T> (ref T, T) where T : class` reach the object-typed icall
	// through `Unsafe.As<T, object>` (System.Threading/Interlocked.cs).
	static void InterlockedGenericTest ()
	{
		Boxed a = new Boxed { Value = 1 };
		Boxed b = new Boxed { Value = 2 };
		Boxed location = a;

		Boxed previous = Interlocked.CompareExchange (ref location, b, a);
		Check ("Interlocked.CompareExchange<T> previous", ReferenceEquals (previous, a));
		Check ("Interlocked.CompareExchange<T> stored", ReferenceEquals (location, b));

		Boxed missedComparand = new Boxed { Value = 3 };
		Boxed missed = Interlocked.CompareExchange (ref location, missedComparand, a);
		Check ("Interlocked.CompareExchange<T> miss previous", ReferenceEquals (missed, b));
		Check ("Interlocked.CompareExchange<T> miss leaves location",
		       ReferenceEquals (location, b));

		Boxed c = new Boxed { Value = 4 };
		Boxed exchanged = Interlocked.Exchange (ref location, c);
		Check ("Interlocked.Exchange<T> previous", ReferenceEquals (exchanged, b));
		Check ("Interlocked.Exchange<T> stored", ReferenceEquals (location, c));
	}
}
