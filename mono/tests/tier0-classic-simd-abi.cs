using System;
using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using Mono.Simd;

// The managed convention gives a whole SIMD value one SSE register, so a
// signature carrying one is the only place a scalar wider than a machine word
// is placed. Five shapes of that: a value of four floats, one of sixteen bytes
// of unsigned int, one behind a field beside an integer, one that arrives past
// the eight floating-point parameter registers and so lands on the stack, and
// a return of two of them at once.
//
// Five more sit at the edges of the two register files: returns of three, four
// and five vectors, the last of which outlasts the vector return registers and
// comes back through a hidden pointer; a return of one vector beside three
// integers, which spends both files at once; and ten vector arguments, the last
// two of which land on 16-byte stack slots.
//
// Each shape has an A and a B copy, the same per-direction split
// tier0-classic-vret-spill.cs uses.
namespace Mono.Tiering {
	static class MonoTier {
		[System.Runtime.CompilerServices.MethodImpl (System.Runtime.CompilerServices.MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
		[System.Runtime.CompilerServices.MethodImpl (System.Runtime.CompilerServices.MethodImplOptions.InternalCall)]
		public static extern int GetTier (IntPtr method);
	}
}

public class Tier0ClassicSimdAbiTest
{
	const int tier0classic = 2;
	const int tier1 = 3;

	struct SimdAbiPair {
		public Vector4 V;
		public long Tag;
	}

	struct SimdAbiTwo {
		public Vector4 A;
		public Vector4 B;
	}

	struct SimdAbiThree {
		public Vector4 A;
		public Vector4 B;
		public Vector4 C;
	}

	struct SimdAbiFour {
		public Vector4 A;
		public Vector4 B;
		public Vector4 C;
		public Vector4 D;
	}

	struct SimdAbiFive {
		public Vector4 A;
		public Vector4 B;
		public Vector4 C;
		public Vector4 D;
		public Vector4 E;
	}

	struct SimdAbiMixed {
		public Vector4 V;
		public long X;
		public long Y;
		public long Z;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Vector4 SimdAbiFloatsA (Vector4 v)
	{
		return new Vector4 (v.W, v.Z, v.Y, v.X);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Vector4 SimdAbiFloatsB (Vector4 v)
	{
		return new Vector4 (v.W, v.Z, v.Y, v.X);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Vector4ui SimdAbiUintsA (Vector4ui v)
	{
		return new Vector4ui (v.W, v.Z, v.Y, v.X);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Vector4ui SimdAbiUintsB (Vector4ui v)
	{
		return new Vector4ui (v.W, v.Z, v.Y, v.X);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static SimdAbiPair SimdAbiPairA (SimdAbiPair p)
	{
		SimdAbiPair r;

		r.V = new Vector4 (p.V.W, p.V.Z, p.V.Y, p.V.X);
		r.Tag = p.Tag * 3;
		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static SimdAbiPair SimdAbiPairB (SimdAbiPair p)
	{
		SimdAbiPair r;

		r.V = new Vector4 (p.V.W, p.V.Z, p.V.Y, p.V.X);
		r.Tag = p.Tag * 3;
		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SimdAbiSpillA (float a, float b, float c, float d,
	                          float e, float f, float g, float h, Vector4 v)
	{
		return (int) (a * 1 + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8)
			+ WeighFloats (v);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SimdAbiSpillB (float a, float b, float c, float d,
	                          float e, float f, float g, float h, Vector4 v)
	{
		return (int) (a * 1 + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8)
			+ WeighFloats (v);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static SimdAbiTwo SimdAbiTwoA (Vector4 v)
	{
		SimdAbiTwo r;

		r.A = v;
		r.B = new Vector4 (v.W, v.Z, v.Y, v.X);
		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static SimdAbiTwo SimdAbiTwoB (Vector4 v)
	{
		SimdAbiTwo r;

		r.A = v;
		r.B = new Vector4 (v.W, v.Z, v.Y, v.X);
		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static SimdAbiThree SimdAbiThreeA (Vector4 v)
	{
		SimdAbiThree r;

		r.A = v;
		r.B = Shift (v, 10);
		r.C = Shift (v, 20);
		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static SimdAbiThree SimdAbiThreeB (Vector4 v)
	{
		SimdAbiThree r;

		r.A = v;
		r.B = Shift (v, 10);
		r.C = Shift (v, 20);
		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static SimdAbiFour SimdAbiFourA (Vector4 v)
	{
		SimdAbiFour r;

		r.A = v;
		r.B = Shift (v, 10);
		r.C = Shift (v, 20);
		r.D = Shift (v, 30);
		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static SimdAbiFour SimdAbiFourB (Vector4 v)
	{
		SimdAbiFour r;

		r.A = v;
		r.B = Shift (v, 10);
		r.C = Shift (v, 20);
		r.D = Shift (v, 30);
		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static SimdAbiFive SimdAbiFiveA (Vector4 v)
	{
		SimdAbiFive r;

		r.A = v;
		r.B = Shift (v, 10);
		r.C = Shift (v, 20);
		r.D = Shift (v, 30);
		r.E = Shift (v, 40);
		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static SimdAbiFive SimdAbiFiveB (Vector4 v)
	{
		SimdAbiFive r;

		r.A = v;
		r.B = Shift (v, 10);
		r.C = Shift (v, 20);
		r.D = Shift (v, 30);
		r.E = Shift (v, 40);
		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static SimdAbiMixed SimdAbiMixedA (Vector4 v, long tag)
	{
		SimdAbiMixed r;

		r.V = new Vector4 (v.W, v.Z, v.Y, v.X);
		r.X = tag;
		r.Y = tag * 2;
		r.Z = tag * 3;
		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static SimdAbiMixed SimdAbiMixedB (Vector4 v, long tag)
	{
		SimdAbiMixed r;

		r.V = new Vector4 (v.W, v.Z, v.Y, v.X);
		r.X = tag;
		r.Y = tag * 2;
		r.Z = tag * 3;
		return r;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SimdAbiTenA (Vector4 a, Vector4 b, Vector4 c, Vector4 d, Vector4 e,
	                        Vector4 f, Vector4 g, Vector4 h, Vector4 i, Vector4 j)
	{
		return WeighFloats (a) * 1 + WeighFloats (b) * 2 + WeighFloats (c) * 3
			+ WeighFloats (d) * 4 + WeighFloats (e) * 5 + WeighFloats (f) * 6
			+ WeighFloats (g) * 7 + WeighFloats (h) * 8 + WeighFloats (i) * 9
			+ WeighFloats (j) * 10;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SimdAbiTenB (Vector4 a, Vector4 b, Vector4 c, Vector4 d, Vector4 e,
	                        Vector4 f, Vector4 g, Vector4 h, Vector4 i, Vector4 j)
	{
		return WeighFloats (a) * 1 + WeighFloats (b) * 2 + WeighFloats (c) * 3
			+ WeighFloats (d) * 4 + WeighFloats (e) * 5 + WeighFloats (f) * 6
			+ WeighFloats (g) * 7 + WeighFloats (h) * 8 + WeighFloats (i) * 9
			+ WeighFloats (j) * 10;
	}

	// Each component is weighed by its own position, so a value that arrived
	// in the wrong register reads as a wrong number rather than as a wrong
	// component.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int WeighFloats (Vector4 v)
	{
		return (int) (v.X * 1 + v.Y * 2 + v.Z * 3 + v.W * 4);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int WeighUints (Vector4ui v)
	{
		return (int) (v.X * 1 + v.Y * 2 + v.Z * 3 + v.W * 4);
	}

	static Vector4 Floats ()
	{
		return new Vector4 (1, 2, 3, 4);
	}

	static Vector4ui Uints ()
	{
		return new Vector4ui (10, 20, 30, 40);
	}

	static Vector4 Shift (Vector4 v, float by)
	{
		return new Vector4 (v.X + by, v.Y + by, v.Z + by, v.W + by);
	}

	static Vector4 Ramp (int k)
	{
		return new Vector4 (k, k + 1, k + 2, k + 3);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SimdAbiCallerA ()
	{
		SimdAbiPair p;
		SimdAbiTwo two;

		p.V = Floats ();
		p.Tag = 7;
		p = SimdAbiPairA (p);
		two = SimdAbiTwoA (Floats ());

		return WeighFloats (SimdAbiFloatsA (Floats ()))
			+ WeighUints (SimdAbiUintsA (Uints ()))
			+ WeighFloats (p.V) + (int) p.Tag
			+ SimdAbiSpillA (1, 2, 3, 4, 5, 6, 7, 8, Floats ())
			+ WeighFloats (two.A) + WeighFloats (two.B);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SimdAbiCallerB ()
	{
		SimdAbiPair p;
		SimdAbiTwo two;

		p.V = Floats ();
		p.Tag = 7;
		p = SimdAbiPairB (p);
		two = SimdAbiTwoB (Floats ());

		return WeighFloats (SimdAbiFloatsB (Floats ()))
			+ WeighUints (SimdAbiUintsB (Uints ()))
			+ WeighFloats (p.V) + (int) p.Tag
			+ SimdAbiSpillB (1, 2, 3, 4, 5, 6, 7, 8, Floats ())
			+ WeighFloats (two.A) + WeighFloats (two.B);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SimdAbiWideCallerA ()
	{
		Vector4 v = Floats ();
		SimdAbiThree three = SimdAbiThreeA (v);
		SimdAbiFour four = SimdAbiFourA (v);
		SimdAbiFive five = SimdAbiFiveA (v);
		SimdAbiMixed mixed = SimdAbiMixedA (v, 7);

		return WeighFloats (three.A) + WeighFloats (three.B) + WeighFloats (three.C)
			+ WeighFloats (four.A) + WeighFloats (four.B) + WeighFloats (four.C)
			+ WeighFloats (four.D)
			+ WeighFloats (five.A) + WeighFloats (five.B) + WeighFloats (five.C)
			+ WeighFloats (five.D) + WeighFloats (five.E)
			+ WeighFloats (mixed.V) + (int) (mixed.X + mixed.Y + mixed.Z)
			+ SimdAbiTenA (Ramp (1), Ramp (2), Ramp (3), Ramp (4), Ramp (5),
			               Ramp (6), Ramp (7), Ramp (8), Ramp (9), Ramp (10));
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SimdAbiWideCallerB ()
	{
		Vector4 v = Floats ();
		SimdAbiThree three = SimdAbiThreeB (v);
		SimdAbiFour four = SimdAbiFourB (v);
		SimdAbiFive five = SimdAbiFiveB (v);
		SimdAbiMixed mixed = SimdAbiMixedB (v, 7);

		return WeighFloats (three.A) + WeighFloats (three.B) + WeighFloats (three.C)
			+ WeighFloats (four.A) + WeighFloats (four.B) + WeighFloats (four.C)
			+ WeighFloats (four.D)
			+ WeighFloats (five.A) + WeighFloats (five.B) + WeighFloats (five.C)
			+ WeighFloats (five.D) + WeighFloats (five.E)
			+ WeighFloats (mixed.V) + (int) (mixed.X + mixed.Y + mixed.Z)
			+ SimdAbiTenB (Ramp (1), Ramp (2), Ramp (3), Ramp (4), Ramp (5),
			               Ramp (6), Ramp (7), Ramp (8), Ramp (9), Ramp (10));
	}

	static IntPtr Handle (string name)
	{
		MethodInfo method = typeof (Tier0ClassicSimdAbiTest).GetMethod (
			name, BindingFlags.Static | BindingFlags.NonPublic);

		return method.MethodHandle.Value;
	}

	static bool Promote (string name)
	{
		return Mono.Tiering.MonoTier.PromoteNow (Handle (name), tier1);
	}

	static int bad = 0;

	static void Check (string what, int got, int want)
	{
		if (got != want) {
			Console.WriteLine ("FAIL: {0} weighed {1}, want {2}", what, got, want);
			bad++;
		}
	}

	static void CheckClassic (string name)
	{
		int tier = Mono.Tiering.MonoTier.GetTier (Handle (name));

		if (tier != tier0classic) {
			Console.WriteLine ("FAIL: {0} ran at tier {1}, want classic tier 0",
			                   name, tier);
			bad++;
		}
	}

	public static int Main ()
	{
		// 20 for the reversed floats, 200 for the reversed uints, 20 + 21 for
		// the pair, 204 + 30 for the spilled call and 30 + 20 for the two
		// returned vectors.
		const int want = 545;
		// 390 for the three vectors, 720 for the four, 1150 for the five, 62
		// for the vector beside three integers and 4950 for the ten arguments.
		const int wantWide = 7272;

		Check ("tier 0 both sides", SimdAbiCallerA (), want);
		Check ("tier 0 both sides", SimdAbiCallerB (), want);
		Check ("wide, tier 0 both sides", SimdAbiWideCallerA (), wantWide);
		Check ("wide, tier 0 both sides", SimdAbiWideCallerB (), wantWide);

		// CheckClassic only holds on the select-all suite arm
		// (mono/tests/runtime-suites.cmake), where every method here compiles
		// through classic.
		if (Environment.GetEnvironmentVariable ("MONO_TIER0_CLASSIC_ALL") != null) {
			CheckClassic ("SimdAbiFloatsA");
			CheckClassic ("SimdAbiUintsA");
			CheckClassic ("SimdAbiPairA");
			CheckClassic ("SimdAbiSpillA");
			CheckClassic ("SimdAbiTwoA");
			CheckClassic ("SimdAbiCallerA");
			CheckClassic ("SimdAbiThreeA");
			CheckClassic ("SimdAbiFourA");
			CheckClassic ("SimdAbiFiveA");
			CheckClassic ("SimdAbiMixedA");
			CheckClassic ("SimdAbiTenA");
			CheckClassic ("SimdAbiWideCallerA");
		}

		if (!Promote ("SimdAbiCallerA") || !Promote ("SimdAbiFloatsB")
		    || !Promote ("SimdAbiUintsB") || !Promote ("SimdAbiPairB")
		    || !Promote ("SimdAbiSpillB") || !Promote ("SimdAbiTwoB")
		    || !Promote ("SimdAbiWideCallerA") || !Promote ("SimdAbiThreeB")
		    || !Promote ("SimdAbiFourB") || !Promote ("SimdAbiFiveB")
		    || !Promote ("SimdAbiMixedB") || !Promote ("SimdAbiTenB")) {
			Console.WriteLine ("FAIL: a method would not compile at tier 1");
			return 1;
		}

		Check ("compiled caller", SimdAbiCallerA (), want);
		Check ("compiled callee", SimdAbiCallerB (), want);
		Check ("wide, compiled caller", SimdAbiWideCallerA (), wantWide);
		Check ("wide, compiled callee", SimdAbiWideCallerB (), wantWide);

		if (!Promote ("SimdAbiFloatsA") || !Promote ("SimdAbiUintsA")
		    || !Promote ("SimdAbiPairA") || !Promote ("SimdAbiSpillA")
		    || !Promote ("SimdAbiTwoA") || !Promote ("SimdAbiCallerB")
		    || !Promote ("SimdAbiThreeA") || !Promote ("SimdAbiFourA")
		    || !Promote ("SimdAbiFiveA") || !Promote ("SimdAbiMixedA")
		    || !Promote ("SimdAbiTenA") || !Promote ("SimdAbiWideCallerB")) {
			Console.WriteLine ("FAIL: a method would not compile at tier 1");
			return 1;
		}

		Check ("tier 1 both sides", SimdAbiCallerA (), want);
		Check ("tier 1 both sides", SimdAbiCallerB (), want);
		Check ("wide, tier 1 both sides", SimdAbiWideCallerA (), wantWide);
		Check ("wide, tier 1 both sides", SimdAbiWideCallerB (), wantWide);

		if (bad != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
