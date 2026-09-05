using System;

// --llvm-opt=-mono-tier0-classic=RetRegs puts the callees and their caller on
// the classic compiler together, which is what a value-type return gathered
// out of registers needs: the callee places it and the caller reads it back.
// A filter naming only the callees answers for nothing, because an interpreted
// caller reaches a callee without asking the backend.
//
// The managed convention answers a value type in up to three integer and two
// SSE registers, so each struct below is one shape of that placement: three
// longs fill the integer file, three ints fill it with a four-byte scalar in
// each register, three longs and a double spend both files at once, and four
// longs is the first shape that comes back through a pointer instead.
public class Tier0ClassicRetRegsTest
{
	struct RetRegsTriple {
		public long A, B, C;
	}

	struct RetRegsInts {
		public int A, B, C;
	}

	struct RetRegsMix {
		public long A, B, C;
		public double D;
	}

	struct RetRegsQuad {
		public long A, B, C, D;
	}

	static RetRegsTriple MakeTriple (long seed)
	{
		RetRegsTriple t;

		t.A = seed;
		t.B = seed + 1;
		t.C = seed + 2;
		return t;
	}

	static RetRegsInts MakeInts (int seed)
	{
		RetRegsInts t;

		t.A = seed;
		t.B = seed + 1;
		t.C = seed + 2;
		return t;
	}

	static RetRegsMix MakeMix (long seed)
	{
		RetRegsMix t;

		t.A = seed;
		t.B = seed + 1;
		t.C = seed + 2;
		t.D = seed + 0.5;
		return t;
	}

	static RetRegsQuad MakeQuad (long seed)
	{
		RetRegsQuad t;

		t.A = seed;
		t.B = seed + 1;
		t.C = seed + 2;
		t.D = seed + 3;
		return t;
	}

	public static int Main ()
	{
		RetRegsTriple t = MakeTriple (70);

		if (t.A != 70 || t.B != 71 || t.C != 72) {
			Console.WriteLine ("FAIL: three longs gave {0} {1} {2}", t.A, t.B, t.C);
			return 1;
		}

		RetRegsInts i = MakeInts (30);

		if (i.A != 30 || i.B != 31 || i.C != 32) {
			Console.WriteLine ("FAIL: three ints gave {0} {1} {2}", i.A, i.B, i.C);
			return 1;
		}

		RetRegsMix m = MakeMix (50);

		if (m.A != 50 || m.B != 51 || m.C != 52 || m.D != 50.5) {
			Console.WriteLine ("FAIL: three longs and a double gave {0} {1} {2} {3}",
			                   m.A, m.B, m.C, m.D);
			return 1;
		}

		RetRegsQuad q = MakeQuad (90);

		if (q.A != 90 || q.B != 91 || q.C != 92 || q.D != 93) {
			Console.WriteLine ("FAIL: four longs gave {0} {1} {2} {3}",
			                   q.A, q.B, q.C, q.D);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
