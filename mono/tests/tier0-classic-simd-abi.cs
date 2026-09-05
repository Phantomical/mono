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

		Check ("tier 0 both sides", SimdAbiCallerA (), want);
		Check ("tier 0 both sides", SimdAbiCallerB (), want);

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
		}

		if (!Promote ("SimdAbiCallerA") || !Promote ("SimdAbiFloatsB")
		    || !Promote ("SimdAbiUintsB") || !Promote ("SimdAbiPairB")
		    || !Promote ("SimdAbiSpillB") || !Promote ("SimdAbiTwoB")) {
			Console.WriteLine ("FAIL: a method would not compile at tier 1");
			return 1;
		}

		Check ("compiled caller", SimdAbiCallerA (), want);
		Check ("compiled callee", SimdAbiCallerB (), want);

		if (!Promote ("SimdAbiFloatsA") || !Promote ("SimdAbiUintsA")
		    || !Promote ("SimdAbiPairA") || !Promote ("SimdAbiSpillA")
		    || !Promote ("SimdAbiTwoA") || !Promote ("SimdAbiCallerB")) {
			Console.WriteLine ("FAIL: a method would not compile at tier 1");
			return 1;
		}

		Check ("tier 1 both sides", SimdAbiCallerA (), want);
		Check ("tier 1 both sides", SimdAbiCallerB (), want);

		if (bad != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}
