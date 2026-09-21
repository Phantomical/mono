using System;
using System.Threading;

/*
 * mono/tests/interlocked.cs and mono/unit-tests/managed/threading.cs already
 * cover CompareExchange and Exchange over int, Exchange over long and
 * object, and Increment, Decrement and Add over int. This covers what those
 * leave out: CompareExchange over long, IntPtr, float and double, Exchange
 * over IntPtr, float and double, Add over long, and Interlocked.Read - the
 * whole set method-to-llvm/interlocked.cpp answers through an atomic
 * instruction instead of the managed-to-native transition every overload
 * took before it.
 */
public class InterlockedScalarWidthsTest
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
		CompareExchangeLongTest ();
		IntPtrTest ();
		FloatTest ();
		DoubleTest ();
		AddLongTest ();
		ReadTest ();

		if (failures != 0) {
			Console.WriteLine ("{0} failures", failures);
			return 1;
		}

		Console.WriteLine ("done!");
		return 0;
	}

	static void CompareExchangeLongTest ()
	{
		long location = 100;

		long previous = Interlocked.CompareExchange (ref location, 200, 100);
		Check ("CompareExchange<long> previous", previous == 100);
		Check ("CompareExchange<long> stored", location == 200);

		long missed = Interlocked.CompareExchange (ref location, 300, 100);
		Check ("CompareExchange<long> miss previous", missed == 200);
		Check ("CompareExchange<long> miss leaves location", location == 200);
	}

	static void IntPtrTest ()
	{
		IntPtr location = (IntPtr) 100;

		IntPtr previous = Interlocked.CompareExchange (ref location, (IntPtr) 200, (IntPtr) 100);
		Check ("CompareExchange<IntPtr> previous", previous == (IntPtr) 100);
		Check ("CompareExchange<IntPtr> stored", location == (IntPtr) 200);

		IntPtr missed = Interlocked.CompareExchange (ref location, (IntPtr) 300, (IntPtr) 100);
		Check ("CompareExchange<IntPtr> miss previous", missed == (IntPtr) 200);
		Check ("CompareExchange<IntPtr> miss leaves location", location == (IntPtr) 200);

		IntPtr exchanged = Interlocked.Exchange (ref location, (IntPtr) 400);
		Check ("Exchange<IntPtr> previous", exchanged == (IntPtr) 200);
		Check ("Exchange<IntPtr> stored", location == (IntPtr) 400);
	}

	static void FloatTest ()
	{
		float location = 1.5f;

		float previous = Interlocked.CompareExchange (ref location, 2.5f, 1.5f);
		Check ("CompareExchange<float> previous", previous == 1.5f);
		Check ("CompareExchange<float> stored", location == 2.5f);

		float missed = Interlocked.CompareExchange (ref location, 3.5f, 1.5f);
		Check ("CompareExchange<float> miss previous", missed == 2.5f);
		Check ("CompareExchange<float> miss leaves location", location == 2.5f);

		float exchanged = Interlocked.Exchange (ref location, 4.5f);
		Check ("Exchange<float> previous", exchanged == 2.5f);
		Check ("Exchange<float> stored", location == 4.5f);

		// -0.0f and 0.0f compare equal, so CompareExchange has to tell them
		// apart by bit pattern the same as a real cmpxchg would, not by ==.
		// Dividing by each gives back which sign the zero carries, with no
		// bit-conversion API this corlib does not have.
		float negZero = -0.0f;

		Interlocked.CompareExchange (ref negZero, 1.0f, 0.0f);
		Check ("CompareExchange<float> distinguishes -0.0 from 0.0",
		       negZero == -0.0f && 1.0f / negZero == float.NegativeInfinity);
	}

	static void DoubleTest ()
	{
		double location = 1.5;

		double previous = Interlocked.CompareExchange (ref location, 2.5, 1.5);
		Check ("CompareExchange<double> previous", previous == 1.5);
		Check ("CompareExchange<double> stored", location == 2.5);

		double missed = Interlocked.CompareExchange (ref location, 3.5, 1.5);
		Check ("CompareExchange<double> miss previous", missed == 2.5);
		Check ("CompareExchange<double> miss leaves location", location == 2.5);

		double exchanged = Interlocked.Exchange (ref location, 4.5);
		Check ("Exchange<double> previous", exchanged == 2.5);
		Check ("Exchange<double> stored", location == 4.5);
	}

	static void AddLongTest ()
	{
		long location = 10;

		long sum = Interlocked.Add (ref location, 5L);
		Check ("Add<long> result", sum == 15);
		Check ("Add<long> stored", location == 15);
	}

	static void ReadTest ()
	{
		long location = 0x1122334455667788L;

		Check ("Interlocked.Read<long>", Interlocked.Read (ref location) == location);
	}
}
