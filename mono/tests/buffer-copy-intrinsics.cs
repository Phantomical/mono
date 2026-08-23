using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

/*
 * The System.Buffer copies the compiled tiers answer with one memory intrinsic
 * instead of the managed copy ladder.
 *
 * The oracle is Reference (), which copies a byte at a time through a
 * temporary and names no method under test. Every sample is compared against
 * it, so a tier that copies the wrong bytes fails whatever the other tiers do.
 * The samples are then compared between the tiers as well, which is what
 * separates a rewrite that is wrong everywhere from one that is wrong at one
 * tier.
 *
 * The rewrite belongs to the method that holds the call site, and that method
 * is in corlib rather than here: Buffer:MemoryCopy and the Marshal read and
 * write pairs. Each sample calls them far past the tier-1 threshold, so they
 * promote on their own. Promoting Sample () alone leaves them where they are.
 *
 * Lengths run across the 32-byte split the ladder branches on, and each one is
 * copied at four offsets, because the ladder picks its loop off the alignment
 * of both ends. A count of zero is in the set: the IL reaches Memcpy with one,
 * and the intrinsic has to stay a no-op there.
 *
 * A negative count is not here. It reaches Memcpy only from inside corlib, and
 * the clamp that answers it is asserted on the IR instead
 * (mono/unit-tests/gtest/llvm/translator-tests.cpp).
 */

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}

static class Program {
	const int Size = 96;

	static int fails;

	static readonly int[] lengths = {
		0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 32, 33, 40, 63, 64,
	};

	/// The distances the overlapping half is exercised over. Each one is less
	/// than a length in the set above, so the two ranges do overlap.
	static readonly int[] distances = { 1, 4, 7, 16 };

	static byte[] Pattern ()
	{
		byte[] bytes = new byte[Size];

		for (int i = 0; i < Size; i++)
			bytes[i] = (byte) (i * 7 + 1);

		return bytes;
	}

	/// One copy, a byte at a time through a temporary, which is right whether
	/// or not the two ranges overlap.
	static void Copy (byte[] into, int destination, byte[] from, int source, int length)
	{
		byte[] held = new byte[length];

		for (int i = 0; i < length; i++)
			held[i] = from[source + i];
		for (int i = 0; i < length; i++)
			into[destination + i] = held[i];
	}

	/// What every copy below has to produce, computed without System.Buffer.
	static byte[] Reference ()
	{
		List<byte> got = new List<byte> ();

		foreach (int length in lengths) {
			for (int offset = 0; offset < 4; offset++) {
				byte[] destination = new byte[Size];

				Copy (destination, offset, Pattern (), offset, length);
				got.AddRange (destination);
			}

			foreach (int distance in distances) {
				byte[] up = Pattern ();

				Copy (up, distance, up, 0, length);
				got.AddRange (up);

				byte[] down = Pattern ();

				Copy (down, 0, down, distance, length);
				got.AddRange (down);
			}
		}

		byte[] raw = Pattern ();

		for (int offset = 0; offset < 8; offset++) {
			got.AddRange (BitConverter.GetBytes (BitConverter.ToInt16 (raw, offset)));
			got.AddRange (BitConverter.GetBytes (BitConverter.ToInt32 (raw, offset)));
			got.AddRange (BitConverter.GetBytes (BitConverter.ToInt64 (raw, offset)));
		}

		for (int offset = 0; offset < 8; offset++) {
			byte[] written = Pattern ();

			Copy (written, offset, BitConverter.GetBytes (0x11223344), 0, 4);
			got.AddRange (written);

			Copy (written, offset, BitConverter.GetBytes (0x5566778899AABBCCL), 0, 8);
			got.AddRange (written);
		}

		return got.ToArray ();
	}

	/*
	 * MemoryCopy takes the source first and the destination second, and it
	 * refuses a copy longer than the room it is promised. Every call below
	 * gives it the room to the end of the array.
	 */
	static unsafe byte[] Sample ()
	{
		List<byte> got = new List<byte> ();

		foreach (int length in lengths) {
			for (int offset = 0; offset < 4; offset++) {
				byte[] source = Pattern ();
				byte[] destination = new byte[Size];

				fixed (byte *s = source, d = destination)
					Buffer.MemoryCopy (s + offset, d + offset,
					                   Size - offset, length);

				got.AddRange (destination);
			}

			foreach (int distance in distances) {
				byte[] up = Pattern ();

				fixed (byte *p = up)
					Buffer.MemoryCopy (p, p + distance,
					                   Size - distance, length);

				got.AddRange (up);

				byte[] down = Pattern ();

				fixed (byte *p = down)
					Buffer.MemoryCopy (p + distance, p, Size, length);

				got.AddRange (down);
			}
		}

		byte[] raw = Pattern ();

		fixed (byte *p = raw) {
			IntPtr address = (IntPtr) p;

			for (int offset = 0; offset < 8; offset++) {
				got.AddRange (BitConverter.GetBytes (Marshal.ReadInt16 (address, offset)));
				got.AddRange (BitConverter.GetBytes (Marshal.ReadInt32 (address, offset)));
				got.AddRange (BitConverter.GetBytes (Marshal.ReadInt64 (address, offset)));
			}
		}

		for (int offset = 0; offset < 8; offset++) {
			byte[] written = Pattern ();

			fixed (byte *p = written) {
				IntPtr address = (IntPtr) p;

				Marshal.WriteInt32 (address, offset, 0x11223344);
				got.AddRange (written);

				Marshal.WriteInt64 (address, offset, 0x5566778899AABBCCL);
				got.AddRange (written);
			}
		}

		return got.ToArray ();
	}

	static void CompareWith (string what, byte[] want, byte[] got)
	{
		if (got.Length != want.Length) {
			Console.WriteLine ("FAIL: {0} gave {1} bytes, wanted {2}",
				what, got.Length, want.Length);
			++fails;
			return;
		}

		for (int i = 0; i < want.Length; i++) {
			if (got[i] == want[i])
				continue;

			Console.WriteLine ("FAIL: {0} byte {1} is {2:x2}, wanted {3:x2}",
				what, i, got[i], want[i]);
			++fails;
			return;
		}
	}

	static bool Promote (MethodInfo method, int tier, string what)
	{
		if (Mono.Tiering.MonoTier.PromoteNow (method.MethodHandle.Value, tier))
			return true;

		Console.WriteLine ("FAIL: {0} would not compile at tier {1}", what, tier);
		++fails;
		return false;
	}

	public static int Main ()
	{
		MethodInfo sample = typeof (Program).GetMethod ("Sample",
			BindingFlags.Static | BindingFlags.NonPublic);

		byte[] want = Reference ();

		CompareWith ("tier 0", want, Sample ());

		if (!Promote (sample, 2, "Sample ()"))
			return 1;

		CompareWith ("tier 1", want, Sample ());

		// Counts for the tier-2 compile to lay the body out against.
		for (int i = 0; i < 200; i++)
			Sample ();

		if (!Promote (sample, 3, "Sample ()"))
			return 1;

		CompareWith ("tier 2", want, Sample ());

		if (fails != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
