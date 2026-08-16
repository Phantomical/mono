// The frame data allocator across frames, and the arguments a newobj moves.
//
// A localloc takes memory from the thread's frame data allocator, which is a
// chain of fragments. The first fragment holds 8168 bytes. A request the current
// fragment cannot hold goes to the next fragment in the chain. If there is no
// next fragment, or the next one has no room left, the allocator frees what is
// there and makes a new fragment.
//
// The arm that reuses the fragment already in the chain needs two rounds. The
// first round makes the spare, and the second round finds it and fits in it.
// signatures.cs holds a nested localloc whose second round is too large for the
// spare, so both of its rounds take the arm that frees the spare. The sizes here
// are the other way round.
//
// newobj puts the new object in two places. One is the call arguments, where it
// is the constructor's this. The other is one slot below, which holds the value
// newobj produces. The arguments the transform staged sit where those two copies
// go, so newobj moves them up first. These tests are the argument shapes
// that move has to carry. opcodes-tail.cs holds the arms where the allocation
// itself is the subject.
//
// A test that makes more than one check returns the number of checks that hold,
// so a failure says how many of them were good.

using System;
using System.Runtime.CompilerServices;

public struct HandlersTailPair {
	public int X, Y;

	public HandlersTailPair (int x, int y) { X = x; Y = y; }
}

// The constructor takes a value type, so the staged arguments are wider than one
// stack slot and the move has to carry all of them.
public struct HandlersTailSpan {
	public HandlersTailPair P;
	public int Z;

	public HandlersTailSpan (HandlersTailPair p, int z) { P = p; Z = z; }
}

// Four longs and an int, so the return slot the value type needs is wider than
// one stack slot. The move distance is that width plus one slot.
public struct HandlersTailBig {
	public long A, B, C, D;
	public int E;

	public HandlersTailBig (long a, long b, long c, long d, int e)
	{
		A = a; B = b; C = c; D = d; E = e;
	}
}

public class HandlersTailNode {
	public HandlersTailNode Next;
	public int V;

	public HandlersTailNode (HandlersTailNode next, int v) { Next = next; V = v; }
}

public class HandlersTailWide {
	public sbyte I1;
	public short I2;
	public int I4;
	public long I8;
	public float R4;
	public double R8;
	public string S;
	public object O;

	public HandlersTailWide (sbyte i1, short i2, int i4, long i8, float r4, double r8,
	                         string s, object o)
	{
		I1 = i1; I2 = i2; I4 = i4; I8 = i8; R4 = r4; R8 = r8; S = s; O = o;
	}
}

class HandlersTailStop : Exception {
}

[Instrumented]
public class HandlersTail {

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int I (int x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long L (long x) { return x; }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static string Str (string x) { return x; }

	static int Ok (bool held) { return held ? 1 : 0; }

	//
	// The frame data allocator.
	//
	// The first fragment holds 8168 bytes. An outer frame of 7000 leaves too
	// little for an inner frame of 1500, which the allocator rounds up to 1504,
	// so the inner one overflows. The spare that overflow makes holds 4072
	// bytes, of which 1504 are spent. The same pair of sizes therefore fits in
	// the spare on every round after the first.
	//

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe void Fill (byte *p, int n)
	{
		for (int i = 0; i < n; i += 512)
			p [i] = (byte) ((i / 512) + 1);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe int Check (byte *p, int n)
	{
		for (int i = 0; i < n; i += 512) {
			if (p [i] != (byte) ((i / 512) + 1))
				return 0;
		}
		return 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe int Inner (int n)
	{
		byte *p = stackalloc byte [n];

		Fill (p, n);
		return Check (p, n);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe int Outer (int outer, int inner)
	{
		byte *p = stackalloc byte [outer];

		Fill (p, outer);
		int deep = Inner (inner);
		// The buffer the outer frame holds is in the fragment the inner frame
		// overflowed out of, so a reuse that moved it shows up here.
		return deep + Check (p, outer);
	}

	// Round one makes the spare fragment. Round two and round three find it and
	// fit in it.
	public static int test_6_frame_data_spare_fragment_is_reused ()
	{
		int a = Outer (I (7000), I (1500));
		int b = Outer (I (7000), I (1500));
		int c = Outer (I (7000), I (1500));

		return a + b + c;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe int InnerThrows (int n)
	{
		byte *p = stackalloc byte [n];

		Fill (p, n);
		throw new HandlersTailStop ();
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static unsafe int OuterCatching (int outer, int inner)
	{
		byte *p = stackalloc byte [outer];

		Fill (p, outer);
		try {
			InnerThrows (inner);
		} catch (HandlersTailStop) {
		}
		return Check (p, outer);
	}

	// The inner frame leaves through an exception rather than through a return,
	// which is a different place in the interpreter to give the memory back from.
	// The allocator has to be in the same state either way, so round two finds
	// the spare that round one made.
	public static int test_2_frame_data_spare_survives_an_unwind ()
	{
		int a = OuterCatching (I (7000), I (1500));
		int b = OuterCatching (I (7000), I (1500));

		return a + b;
	}

	// The middle round asks for more than the spare has left, so the spare goes
	// and a new fragment takes its place. The last round fits in the new one.
	public static int test_6_frame_data_spare_is_dropped_then_remade ()
	{
		int a = Outer (I (7000), I (1500));
		int b = Outer (I (7000), I (3000));
		int c = Outer (I (7000), I (1000));

		return a + b + c;
	}

	//
	// The arguments a newobj stages.
	//

	// The first argument of each allocation is the object the allocation inside
	// it made, so the move carries a live object reference.
	public static int test_6_newobj_nested_in_its_own_argument ()
	{
		HandlersTailNode n = new HandlersTailNode (
			new HandlersTailNode (new HandlersTailNode (null, I (1)), I (2)), I (3));

		return n.V + n.Next.V + n.Next.Next.V;
	}

	// Eight arguments of six widths, so the move covers a run of slots rather
	// than one slot.
	public static int test_8_newobj_with_a_wide_argument_list ()
	{
		HandlersTailWide w = new HandlersTailWide ((sbyte) I (-3), (short) I (-300), I (7),
		                                           L (-1234567890123L), 1.5f, 2.25,
		                                           Str ("abcd"), Str ("xy"));

		return Ok (w.I1 == -3) + Ok (w.I2 == -300) + Ok (w.I4 == 7)
		     + Ok (w.I8 == -1234567890123L) + Ok (w.R4 == 1.5f) + Ok (w.R8 == 2.25)
		     + Ok (w.S.Length == 4) + Ok (((string) w.O).Length == 2);
	}

	// A value type constructor gets the address of the return slot in place of an
	// object, and the argument here is itself a value type from a constructor.
	public static int test_7_newobj_vt_takes_a_struct_argument ()
	{
		HandlersTailSpan s = new HandlersTailSpan (new HandlersTailPair (I (1), I (2)), I (4));

		return s.P.X + s.P.Y + s.Z;
	}

	// The return slot is wider than one stack slot, so the arguments move by
	// that width plus one slot rather than by two slots.
	public static int test_12_newobj_vt_wider_than_a_stack_slot ()
	{
		HandlersTailBig b = new HandlersTailBig (L (1), L (2), L (3), L (4), I (2));

		return (int) (b.A + b.B + b.C + b.D) + b.E;
	}
}
