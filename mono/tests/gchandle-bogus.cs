// Every GCHandle entry point, reached with a value that was never a handle.
//
// A failure is the process going down inside the collector's handle code.
// Returning and throwing both pass: what matters is that the runtime does not
// follow the value.
//
// A boehm handle is a pointer into a HandleData block, so the bytes below are
// laid out as one. A plausible type and size get past the cheap checks and
// reach the bitmap read. Sgen packs a slot and a type into one value, where the
// same address is an index far past the end of the slot array.
using System;
using System.Runtime.InteropServices;

public static class GCHandleBogus {
	// struct HandleData, mono/metadata/boehm-gc.c.
	const int BitmapOffset = 0x10;
	const int SizeOffset = 0x24;
	const int TypeOffset = 0x2c;
	const int EntriesOffset = 0x38;
	const int BlockAlignment = 1 << 13;

	const int HandleNormal = 2;
	const int Slot = 411;

	// Two IEEE floats, so the bytes are the shape a vertex buffer leaves behind,
	// and not an address anything has mapped.
	const long UnmappedBitmap = 0x3e42437a3de2c365;

	static int failures;

	static void Survives (string name, Action body)
	{
		try {
			body ();
			Console.WriteLine ("  OK    {0}: returned", name);
		} catch (ArgumentException e) {
			Console.WriteLine ("  OK    {0}: rejected, {1}", name, e.Message);
		} catch (Exception e) {
			Console.WriteLine ("  FAIL  {0}: {1}: {2}", name, e.GetType ().Name, e.Message);
			failures++;
		}
	}

	static void Works (string name, Action body)
	{
		try {
			body ();
			Console.WriteLine ("  OK    {0}", name);
		} catch (Exception e) {
			Console.WriteLine ("  FAIL  {0}: {1}: {2}", name, e.GetType ().Name, e.Message);
			failures++;
		}
	}

	// A GCHandle is one IntPtr. A caller holding one in native memory reaches
	// Free () with the struct itself, so the domain check that guards
	// FromIntPtr never runs.
	static unsafe GCHandle Reinterpret (IntPtr value)
	{
		GCHandle h = default (GCHandle);
		*(IntPtr*)&h = value;
		return h;
	}

	public static unsafe int Main ()
	{
		var backing = new byte [1 << 17];
		GCHandle pin = GCHandle.Alloc (backing, GCHandleType.Pinned);

		byte *start = (byte *)pin.AddrOfPinnedObject ();
		byte *block = (byte *)(((long)start + BlockAlignment - 1) & ~((long)BlockAlignment - 1));

		*(long *)(block + BitmapOffset) = UnmappedBitmap;
		*(int *)(block + SizeOffset) = 992;
		*(block + TypeOffset) = HandleNormal;

		IntPtr bogus = (IntPtr)(block + EntriesOffset + Slot * sizeof (void *));

		Survives ("FromIntPtr", () => { GCHandle h = GCHandle.FromIntPtr (bogus); GC.KeepAlive (h); });
		Survives ("Free", () => Reinterpret (bogus).Free ());
		Survives ("Target get", () => GC.KeepAlive (Reinterpret (bogus).Target));
		Survives ("Target set", () => { GCHandle h = Reinterpret (bogus); h.Target = "x"; });
		Survives ("IsAllocated", () => { if (Reinterpret (bogus).IsAllocated) { } });

		Works ("a real handle still round-trips", () => {
			var live = new object ();
			GCHandle h = GCHandle.Alloc (live);

			if (!ReferenceEquals (h.Target, live))
				throw new Exception ("Target came back wrong");

			h.Target = backing;
			if (!ReferenceEquals (h.Target, backing))
				throw new Exception ("Target did not take");

			h.Free ();
			if (h.IsAllocated)
				throw new Exception ("still allocated after Free");
		});

		pin.Free ();

		Console.WriteLine (failures == 0 ? "OK" : failures + " FAILED");
		return failures == 0 ? 0 : 1;
	}
}
