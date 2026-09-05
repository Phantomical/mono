using System;
using System.Reflection;

// --llvm-opt=-mono-tier0-classic=WideArgs puts this whole class on the classic
// compiler. Both methods below have a short body and a wide signature, which is
// what the prologue and the epilogue size their code buffers against: the
// buffer comes from the method's IL length, and the moves that save the
// arguments and gather the return come from the layout of the value types the
// signature carries.
//
// WideArgsPad is a byte, seven bytes of padding and a long, which the managed
// convention places as nine scalars. Sixteen of them is 144 moves in front of a
// body of a few hundred IL bytes.
//
// WideArgsTriple is three longs, which comes back in three return registers
// rather than the two an eightbyte view can hold. Reflection is what calls the
// method that returns one, so the caller gathering those registers is the
// runtime-invoke wrapper rather than a second classic body.
public class Tier0ClassicWideArgsTest
{
	struct WideArgsPad {
		public byte Tag;
		public long Value;
	}

	struct WideArgsTriple {
		public long A, B, C;
	}

	static WideArgsPad Pad (int i)
	{
		WideArgsPad p;

		p.Tag = (byte) i;
		p.Value = i * 1000;
		return p;
	}

	static long WideArgsSum (WideArgsPad a1, WideArgsPad a2, WideArgsPad a3, WideArgsPad a4,
	                         WideArgsPad a5, WideArgsPad a6, WideArgsPad a7, WideArgsPad a8,
	                         WideArgsPad a9, WideArgsPad a10, WideArgsPad a11, WideArgsPad a12,
	                         WideArgsPad a13, WideArgsPad a14, WideArgsPad a15, WideArgsPad a16)
	{
		return a1.Tag + a1.Value + a2.Tag + a2.Value + a3.Tag + a3.Value + a4.Tag + a4.Value
			+ a5.Tag + a5.Value + a6.Tag + a6.Value + a7.Tag + a7.Value + a8.Tag + a8.Value
			+ a9.Tag + a9.Value + a10.Tag + a10.Value + a11.Tag + a11.Value + a12.Tag + a12.Value
			+ a13.Tag + a13.Value + a14.Tag + a14.Value + a15.Tag + a15.Value + a16.Tag + a16.Value;
	}

	static WideArgsTriple WideArgsMake (long seed)
	{
		WideArgsTriple t;

		t.A = seed;
		t.B = seed + 1;
		t.C = seed + 2;
		return t;
	}

	public static int Main ()
	{
		long want = 0;

		for (int i = 1; i <= 16; i++)
			want += i + i * 1000;

		long got = WideArgsSum (Pad (1), Pad (2), Pad (3), Pad (4), Pad (5), Pad (6),
		                        Pad (7), Pad (8), Pad (9), Pad (10), Pad (11), Pad (12),
		                        Pad (13), Pad (14), Pad (15), Pad (16));

		if (got != want) {
			Console.WriteLine ("FAIL: sum of sixteen value types gave {0}, want {1}", got, want);
			return 1;
		}

		MethodInfo make = typeof (Tier0ClassicWideArgsTest).GetMethod (
			"WideArgsMake", BindingFlags.Static | BindingFlags.NonPublic);
		WideArgsTriple t = (WideArgsTriple) make.Invoke (null, new object [] { 70L });

		if (t.A != 70 || t.B != 71 || t.C != 72) {
			Console.WriteLine ("FAIL: three-register return gave {0} {1} {2}", t.A, t.B, t.C);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
