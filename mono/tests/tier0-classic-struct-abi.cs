using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

// Managed value types crossing every call boundary between the engines in one
// process: the interpreter, classic tier 0, LLVM tier 1, and the invoke
// wrapper the runtime compiles for a delegate.
//
// Three role classes hold the same set of takers and makers, so one struct
// kind can be driven in either direction. StructAbiClassic is what
// --llvm-opt=-mono-tier0-classic names, StructAbiFast is promoted to tier 1
// by hand, and StructAbiInterp is never asked for at all, which is what
// leaves it interpreted.
//
// Every taker reads a scalar in front of its struct and a scalar behind it
// and answers with their sum. A convention that spends a different number of
// registers on the struct than the other end expects moves those two, so a
// shifted argument shows up as a wrong answer rather than as a wrong field.
namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern int GetTier (IntPtr method);
	}
}

// Two i32 leaves in eight bytes.
public struct S8i {
	public int a, b;

	public bool Ok (int seed) { return a == seed && b == seed + 1; }
}

// Two float leaves in eight bytes, which ride the SSE file rather than the
// integer one.
public struct S8f {
	public float x, y;

	public bool Ok (int seed) { return x == seed && y == seed + 1; }
}

// Four bytes, smaller than one register.
public struct S4 {
	public int a;

	public bool Ok (int seed) { return a == seed; }
}

// Sixteen bytes of references: the size class where a return either comes
// back in two registers or falls back to a hidden pointer.
public struct S16 {
	public string s;
	public object o;

	public bool Ok (int seed)
	{
		string boxed = o as string;

		return s == "s" + seed && boxed == "o" + seed;
	}
}

// Thirty-two bytes, which is System.ParamsArray's shape.
public struct S32 {
	public string a, b, c, d;

	public bool Ok (int seed)
	{
		return a == "a" + seed && b == "b" + seed && c == "c" + seed
		       && d == "d" + seed;
	}
}

// A byte, three bytes of layout no field claims, and an int.
public struct SPad {
	public byte b;
	public int i;

	public bool Ok (int seed) { return b == (byte) seed && i == seed + 1; }
}

// A struct of structs, whose leaves flatten into the enclosing one's.
public struct SNest {
	public S8i lo;
	public S8f hi;

	public bool Ok (int seed) { return lo.Ok (seed) && hi.Ok (seed + 2); }
}

// No fields at all, which is still layout and so still an argument.
public struct SEmpty {
}

public class StructAbiClassic {
	public static int TakeS4 (int tag, S4 v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeS8i (int tag, S8i v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeS8f (int tag, S8f v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeS16 (int tag, S16 v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeS32 (int tag, S32 v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeSPad (int tag, SPad v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeSNest (int tag, SNest v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeSEmpty (int tag, SEmpty v, int tail)
	{
		return tag + tail;
	}

	// One signature that runs the integer registers out in the middle of a
	// struct, which is where the convention splits one between registers and
	// the stack.
	public static int TakeMix (int tag, S8i a, S8f b, S16 c, S32 d, SPad e,
	                           double f, string g, int tail)
	{
		if (!a.Ok (7) || !b.Ok (7) || !c.Ok (7) || !d.Ok (7) || !e.Ok (7))
			return -1;
		if (f != 2.5 || g != "g")
			return -1;
		return tag + tail;
	}

	public static S4 MakeS4 (int seed) { return StructAbiValues.NewS4 (seed); }
	public static S8i MakeS8i (int seed) { return StructAbiValues.NewS8i (seed); }
	public static S8f MakeS8f (int seed) { return StructAbiValues.NewS8f (seed); }
	public static S16 MakeS16 (int seed) { return StructAbiValues.NewS16 (seed); }
	public static S32 MakeS32 (int seed) { return StructAbiValues.NewS32 (seed); }
	public static SPad MakeSPad (int seed) { return StructAbiValues.NewSPad (seed); }
	public static SNest MakeSNest (int seed) { return StructAbiValues.NewSNest (seed); }

	// A struct return whose hidden pointer, where there is one, sits behind
	// an argument rather than in front of it.
	public static S32 MakeS32Behind (int seed, string label)
	{
		S32 v = StructAbiValues.NewS32 (seed);

		v.a = label;
		return v;
	}

	// Reflection enters this one, which compiles it. Every call it makes then
	// compiles its callee the same way. That is what puts a classic body
	// under each of the names above before anything else reaches them.
	public static int Warm ()
	{
		return TakeS4 (1, StructAbiValues.A4, 1) + TakeS8i (1, StructAbiValues.A8i, 1)
		       + TakeS8f (1, StructAbiValues.A8f, 1) + TakeS16 (1, StructAbiValues.A16, 1)
		       + TakeS32 (1, StructAbiValues.A32, 1) + TakeSPad (1, StructAbiValues.APad, 1)
		       + TakeSNest (1, StructAbiValues.ANest, 1)
		       + TakeSEmpty (1, StructAbiValues.AEmpty, 1)
		       + TakeMix (1, StructAbiValues.A8i, StructAbiValues.A8f,
		                  StructAbiValues.A16, StructAbiValues.A32,
		                  StructAbiValues.APad, 2.5, "g", 1)
		       + MakeS4 (11).a + MakeS8i (11).a + (int) MakeS8f (11).x
		       + MakeS16 (11).s.Length + MakeS32 (11).a.Length + MakeSPad (11).b
		       + MakeSNest (11).lo.a + MakeS32Behind (11, "label").a.Length;
	}

	public static void DriveFast ()
	{
		const string w = "classic -> tier1 ";

		StructAbiCheck.Answer (w + "TakeS4", StructAbiFast.TakeS4 (StructAbiCheck.Tag, StructAbiValues.A4, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeS8i", StructAbiFast.TakeS8i (StructAbiCheck.Tag, StructAbiValues.A8i, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeS8f", StructAbiFast.TakeS8f (StructAbiCheck.Tag, StructAbiValues.A8f, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeS16", StructAbiFast.TakeS16 (StructAbiCheck.Tag, StructAbiValues.A16, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeS32", StructAbiFast.TakeS32 (StructAbiCheck.Tag, StructAbiValues.A32, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeSPad", StructAbiFast.TakeSPad (StructAbiCheck.Tag, StructAbiValues.APad, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeSNest", StructAbiFast.TakeSNest (StructAbiCheck.Tag, StructAbiValues.ANest, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeSEmpty", StructAbiFast.TakeSEmpty (StructAbiCheck.Tag, StructAbiValues.AEmpty, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeMix", StructAbiFast.TakeMix (StructAbiCheck.Tag, StructAbiValues.A8i, StructAbiValues.A8f, StructAbiValues.A16, StructAbiValues.A32, StructAbiValues.APad, 2.5, "g", StructAbiCheck.Tail));

		StructAbiCheck.Truth (w + "MakeS4", StructAbiFast.MakeS4 (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS8i", StructAbiFast.MakeS8i (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS8f", StructAbiFast.MakeS8f (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS16", StructAbiFast.MakeS16 (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS32", StructAbiFast.MakeS32 (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeSPad", StructAbiFast.MakeSPad (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeSNest", StructAbiFast.MakeSNest (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS32Behind", StructAbiFast.MakeS32Behind (11, "label").a == "label");
	}

	public static void DriveInterp ()
	{
		const string w = "classic -> interp ";

		StructAbiCheck.Answer (w + "TakeS4", StructAbiInterp.TakeS4 (StructAbiCheck.Tag, StructAbiValues.A4, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeS8i", StructAbiInterp.TakeS8i (StructAbiCheck.Tag, StructAbiValues.A8i, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeS8f", StructAbiInterp.TakeS8f (StructAbiCheck.Tag, StructAbiValues.A8f, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeS16", StructAbiInterp.TakeS16 (StructAbiCheck.Tag, StructAbiValues.A16, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeS32", StructAbiInterp.TakeS32 (StructAbiCheck.Tag, StructAbiValues.A32, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeSPad", StructAbiInterp.TakeSPad (StructAbiCheck.Tag, StructAbiValues.APad, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeSNest", StructAbiInterp.TakeSNest (StructAbiCheck.Tag, StructAbiValues.ANest, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeSEmpty", StructAbiInterp.TakeSEmpty (StructAbiCheck.Tag, StructAbiValues.AEmpty, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeMix", StructAbiInterp.TakeMix (StructAbiCheck.Tag, StructAbiValues.A8i, StructAbiValues.A8f, StructAbiValues.A16, StructAbiValues.A32, StructAbiValues.APad, 2.5, "g", StructAbiCheck.Tail));

		StructAbiCheck.Truth (w + "MakeS4", StructAbiInterp.MakeS4 (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS8i", StructAbiInterp.MakeS8i (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS8f", StructAbiInterp.MakeS8f (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS16", StructAbiInterp.MakeS16 (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS32", StructAbiInterp.MakeS32 (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeSPad", StructAbiInterp.MakeSPad (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeSNest", StructAbiInterp.MakeSNest (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS32Behind", StructAbiInterp.MakeS32Behind (11, "label").a == "label");
	}

	public static void DriveDelegates ()
	{
		StructAbiCheck.Delegates ("classic -> invoke wrapper ");
	}
}

public class StructAbiFast {
	public static int TakeS4 (int tag, S4 v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeS8i (int tag, S8i v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeS8f (int tag, S8f v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeS16 (int tag, S16 v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeS32 (int tag, S32 v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeSPad (int tag, SPad v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeSNest (int tag, SNest v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeSEmpty (int tag, SEmpty v, int tail)
	{
		return tag + tail;
	}

	public static int TakeMix (int tag, S8i a, S8f b, S16 c, S32 d, SPad e,
	                           double f, string g, int tail)
	{
		if (!a.Ok (7) || !b.Ok (7) || !c.Ok (7) || !d.Ok (7) || !e.Ok (7))
			return -1;
		if (f != 2.5 || g != "g")
			return -1;
		return tag + tail;
	}

	public static S4 MakeS4 (int seed) { return StructAbiValues.NewS4 (seed); }
	public static S8i MakeS8i (int seed) { return StructAbiValues.NewS8i (seed); }
	public static S8f MakeS8f (int seed) { return StructAbiValues.NewS8f (seed); }
	public static S16 MakeS16 (int seed) { return StructAbiValues.NewS16 (seed); }
	public static S32 MakeS32 (int seed) { return StructAbiValues.NewS32 (seed); }
	public static SPad MakeSPad (int seed) { return StructAbiValues.NewSPad (seed); }
	public static SNest MakeSNest (int seed) { return StructAbiValues.NewSNest (seed); }

	public static S32 MakeS32Behind (int seed, string label)
	{
		S32 v = StructAbiValues.NewS32 (seed);

		v.a = label;
		return v;
	}

	public static void DriveClassic ()
	{
		const string w = "tier1 -> classic ";

		StructAbiCheck.Answer (w + "TakeS4", StructAbiClassic.TakeS4 (StructAbiCheck.Tag, StructAbiValues.A4, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeS8i", StructAbiClassic.TakeS8i (StructAbiCheck.Tag, StructAbiValues.A8i, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeS8f", StructAbiClassic.TakeS8f (StructAbiCheck.Tag, StructAbiValues.A8f, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeS16", StructAbiClassic.TakeS16 (StructAbiCheck.Tag, StructAbiValues.A16, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeS32", StructAbiClassic.TakeS32 (StructAbiCheck.Tag, StructAbiValues.A32, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeSPad", StructAbiClassic.TakeSPad (StructAbiCheck.Tag, StructAbiValues.APad, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeSNest", StructAbiClassic.TakeSNest (StructAbiCheck.Tag, StructAbiValues.ANest, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeSEmpty", StructAbiClassic.TakeSEmpty (StructAbiCheck.Tag, StructAbiValues.AEmpty, StructAbiCheck.Tail));
		StructAbiCheck.Answer (w + "TakeMix", StructAbiClassic.TakeMix (StructAbiCheck.Tag, StructAbiValues.A8i, StructAbiValues.A8f, StructAbiValues.A16, StructAbiValues.A32, StructAbiValues.APad, 2.5, "g", StructAbiCheck.Tail));

		StructAbiCheck.Truth (w + "MakeS4", StructAbiClassic.MakeS4 (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS8i", StructAbiClassic.MakeS8i (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS8f", StructAbiClassic.MakeS8f (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS16", StructAbiClassic.MakeS16 (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS32", StructAbiClassic.MakeS32 (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeSPad", StructAbiClassic.MakeSPad (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeSNest", StructAbiClassic.MakeSNest (11).Ok (11));
		StructAbiCheck.Truth (w + "MakeS32Behind", StructAbiClassic.MakeS32Behind (11, "label").a == "label");
	}
}

public class StructAbiInterp {
	public static int TakeS4 (int tag, S4 v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeS8i (int tag, S8i v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeS8f (int tag, S8f v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeS16 (int tag, S16 v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeS32 (int tag, S32 v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeSPad (int tag, SPad v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeSNest (int tag, SNest v, int tail)
	{
		return v.Ok (7) ? tag + tail : -1;
	}

	public static int TakeSEmpty (int tag, SEmpty v, int tail)
	{
		return tag + tail;
	}

	public static int TakeMix (int tag, S8i a, S8f b, S16 c, S32 d, SPad e,
	                           double f, string g, int tail)
	{
		if (!a.Ok (7) || !b.Ok (7) || !c.Ok (7) || !d.Ok (7) || !e.Ok (7))
			return -1;
		if (f != 2.5 || g != "g")
			return -1;
		return tag + tail;
	}

	public static S4 MakeS4 (int seed) { return StructAbiValues.NewS4 (seed); }
	public static S8i MakeS8i (int seed) { return StructAbiValues.NewS8i (seed); }
	public static S8f MakeS8f (int seed) { return StructAbiValues.NewS8f (seed); }
	public static S16 MakeS16 (int seed) { return StructAbiValues.NewS16 (seed); }
	public static S32 MakeS32 (int seed) { return StructAbiValues.NewS32 (seed); }
	public static SPad MakeSPad (int seed) { return StructAbiValues.NewSPad (seed); }
	public static SNest MakeSNest (int seed) { return StructAbiValues.NewSNest (seed); }

	public static S32 MakeS32Behind (int seed, string label)
	{
		S32 v = StructAbiValues.NewS32 (seed);

		v.a = label;
		return v;
	}
}

// The arguments every taker is handed, built once. A static field read hands
// the value over without a call, so the only boundary a taker's answer
// describes is its own.
public static class StructAbiValues {
	public static S4 A4 = NewS4 (7);
	public static S8i A8i = NewS8i (7);
	public static S8f A8f = NewS8f (7);
	public static S16 A16 = NewS16 (7);
	public static S32 A32 = NewS32 (7);
	public static SPad APad = NewSPad (7);
	public static SNest ANest = NewSNest (7);
	public static SEmpty AEmpty;

	public static S4 NewS4 (int seed)
	{
		S4 v;

		v.a = seed;
		return v;
	}

	public static S8i NewS8i (int seed)
	{
		S8i v;

		v.a = seed;
		v.b = seed + 1;
		return v;
	}

	public static S8f NewS8f (int seed)
	{
		S8f v;

		v.x = seed;
		v.y = seed + 1;
		return v;
	}

	public static S16 NewS16 (int seed)
	{
		S16 v;

		v.s = "s" + seed;
		v.o = "o" + seed;
		return v;
	}

	public static S32 NewS32 (int seed)
	{
		S32 v;

		v.a = "a" + seed;
		v.b = "b" + seed;
		v.c = "c" + seed;
		v.d = "d" + seed;
		return v;
	}

	public static SPad NewSPad (int seed)
	{
		SPad v;

		v.b = (byte) seed;
		v.i = seed + 1;
		return v;
	}

	public static SNest NewSNest (int seed)
	{
		SNest v;

		v.lo = NewS8i (seed);
		v.hi = NewS8f (seed + 2);
		return v;
	}
}

public delegate S4 MakeS4Delegate (int seed);
public delegate S8f MakeS8fDelegate (int seed);
public delegate S16 MakeS16Delegate (int seed);
public delegate S32 MakeS32Delegate (int seed);

public static class StructAbiCheck {
	public const int Tag = 100;
	public const int Tail = 23;
	public const int Want = Tag + Tail;

	static int failures;

	public static int Failures { get { return failures; } }

	public static void Truth (string what, bool ok)
	{
		if (!ok) {
			Console.WriteLine ("FAIL: {0}", what);
			failures++;
		}
	}

	public static void Answer (string what, int got)
	{
		if (got != Want) {
			Console.WriteLine ("FAIL: {0}: got {1}, want {2}", what, got, Want);
			failures++;
		}
	}

	// A delegate over a struct-returning method. The invoke wrapper the
	// runtime builds for one is no tier-0 method, so it is compiled by LLVM
	// whatever the classic filter says, and the caller has to speak the
	// convention that wrapper was built with.
	public static void Delegates (string w)
	{
		MakeS4Delegate d4 = StructAbiClassic.MakeS4;
		MakeS8fDelegate d8f = StructAbiClassic.MakeS8f;
		MakeS16Delegate d16 = StructAbiClassic.MakeS16;
		MakeS32Delegate d32 = StructAbiClassic.MakeS32;

		Truth (w + "MakeS4", d4 (11).Ok (11));
		Truth (w + "MakeS8f", d8f (11).Ok (11));
		Truth (w + "MakeS16", d16 (11).Ok (11));
		Truth (w + "MakeS32", d32 (11).Ok (11));
	}

	public static void SweepClassic ()
	{
		const string w = "interp -> classic ";

		Answer (w + "TakeS4", StructAbiClassic.TakeS4 (Tag, StructAbiValues.A4, Tail));
		Answer (w + "TakeS8i", StructAbiClassic.TakeS8i (Tag, StructAbiValues.A8i, Tail));
		Answer (w + "TakeS8f", StructAbiClassic.TakeS8f (Tag, StructAbiValues.A8f, Tail));
		Answer (w + "TakeS16", StructAbiClassic.TakeS16 (Tag, StructAbiValues.A16, Tail));
		Answer (w + "TakeS32", StructAbiClassic.TakeS32 (Tag, StructAbiValues.A32, Tail));
		Answer (w + "TakeSPad", StructAbiClassic.TakeSPad (Tag, StructAbiValues.APad, Tail));
		Answer (w + "TakeSNest", StructAbiClassic.TakeSNest (Tag, StructAbiValues.ANest, Tail));
		Answer (w + "TakeSEmpty", StructAbiClassic.TakeSEmpty (Tag, StructAbiValues.AEmpty, Tail));
		Answer (w + "TakeMix", StructAbiClassic.TakeMix (Tag, StructAbiValues.A8i, StructAbiValues.A8f, StructAbiValues.A16, StructAbiValues.A32, StructAbiValues.APad, 2.5, "g", Tail));

		Truth (w + "MakeS4", StructAbiClassic.MakeS4 (11).Ok (11));
		Truth (w + "MakeS8i", StructAbiClassic.MakeS8i (11).Ok (11));
		Truth (w + "MakeS8f", StructAbiClassic.MakeS8f (11).Ok (11));
		Truth (w + "MakeS16", StructAbiClassic.MakeS16 (11).Ok (11));
		Truth (w + "MakeS32", StructAbiClassic.MakeS32 (11).Ok (11));
		Truth (w + "MakeSPad", StructAbiClassic.MakeSPad (11).Ok (11));
		Truth (w + "MakeSNest", StructAbiClassic.MakeSNest (11).Ok (11));
		Truth (w + "MakeS32Behind", StructAbiClassic.MakeS32Behind (11, "label").a == "label");
	}

	public static void SweepFast ()
	{
		const string w = "interp -> tier1 ";

		Answer (w + "TakeS4", StructAbiFast.TakeS4 (Tag, StructAbiValues.A4, Tail));
		Answer (w + "TakeS8i", StructAbiFast.TakeS8i (Tag, StructAbiValues.A8i, Tail));
		Answer (w + "TakeS8f", StructAbiFast.TakeS8f (Tag, StructAbiValues.A8f, Tail));
		Answer (w + "TakeS16", StructAbiFast.TakeS16 (Tag, StructAbiValues.A16, Tail));
		Answer (w + "TakeS32", StructAbiFast.TakeS32 (Tag, StructAbiValues.A32, Tail));
		Answer (w + "TakeSPad", StructAbiFast.TakeSPad (Tag, StructAbiValues.APad, Tail));
		Answer (w + "TakeSNest", StructAbiFast.TakeSNest (Tag, StructAbiValues.ANest, Tail));
		Answer (w + "TakeSEmpty", StructAbiFast.TakeSEmpty (Tag, StructAbiValues.AEmpty, Tail));
		Answer (w + "TakeMix", StructAbiFast.TakeMix (Tag, StructAbiValues.A8i, StructAbiValues.A8f, StructAbiValues.A16, StructAbiValues.A32, StructAbiValues.APad, 2.5, "g", Tail));

		Truth (w + "MakeS4", StructAbiFast.MakeS4 (11).Ok (11));
		Truth (w + "MakeS8i", StructAbiFast.MakeS8i (11).Ok (11));
		Truth (w + "MakeS8f", StructAbiFast.MakeS8f (11).Ok (11));
		Truth (w + "MakeS16", StructAbiFast.MakeS16 (11).Ok (11));
		Truth (w + "MakeS32", StructAbiFast.MakeS32 (11).Ok (11));
		Truth (w + "MakeSPad", StructAbiFast.MakeSPad (11).Ok (11));
		Truth (w + "MakeSNest", StructAbiFast.MakeSNest (11).Ok (11));
		Truth (w + "MakeS32Behind", StructAbiFast.MakeS32Behind (11, "label").a == "label");
	}

	public static void SweepInterp ()
	{
		const string w = "interp -> interp ";

		Answer (w + "TakeS4", StructAbiInterp.TakeS4 (Tag, StructAbiValues.A4, Tail));
		Answer (w + "TakeS8i", StructAbiInterp.TakeS8i (Tag, StructAbiValues.A8i, Tail));
		Answer (w + "TakeS8f", StructAbiInterp.TakeS8f (Tag, StructAbiValues.A8f, Tail));
		Answer (w + "TakeS16", StructAbiInterp.TakeS16 (Tag, StructAbiValues.A16, Tail));
		Answer (w + "TakeS32", StructAbiInterp.TakeS32 (Tag, StructAbiValues.A32, Tail));
		Answer (w + "TakeSPad", StructAbiInterp.TakeSPad (Tag, StructAbiValues.APad, Tail));
		Answer (w + "TakeSNest", StructAbiInterp.TakeSNest (Tag, StructAbiValues.ANest, Tail));
		Answer (w + "TakeSEmpty", StructAbiInterp.TakeSEmpty (Tag, StructAbiValues.AEmpty, Tail));
		Answer (w + "TakeMix", StructAbiInterp.TakeMix (Tag, StructAbiValues.A8i, StructAbiValues.A8f, StructAbiValues.A16, StructAbiValues.A32, StructAbiValues.APad, 2.5, "g", Tail));

		Truth (w + "MakeS4", StructAbiInterp.MakeS4 (11).Ok (11));
		Truth (w + "MakeS8i", StructAbiInterp.MakeS8i (11).Ok (11));
		Truth (w + "MakeS8f", StructAbiInterp.MakeS8f (11).Ok (11));
		Truth (w + "MakeS16", StructAbiInterp.MakeS16 (11).Ok (11));
		Truth (w + "MakeS32", StructAbiInterp.MakeS32 (11).Ok (11));
		Truth (w + "MakeSPad", StructAbiInterp.MakeSPad (11).Ok (11));
		Truth (w + "MakeSNest", StructAbiInterp.MakeSNest (11).Ok (11));
		Truth (w + "MakeS32Behind", StructAbiInterp.MakeS32Behind (11, "label").a == "label");
	}
}

public class Tier0ClassicStructAbiTest {
	const int tier1 = 3;

	static readonly string[] driven = {
		"TakeS4", "TakeS8i", "TakeS8f", "TakeS16", "TakeS32", "TakeSPad",
		"TakeSNest", "TakeSEmpty", "TakeMix", "MakeS4", "MakeS8i", "MakeS8f",
		"MakeS16", "MakeS32", "MakeSPad", "MakeSNest", "MakeS32Behind",
		"DriveClassic",
	};

	static bool promote (string name)
	{
		MethodInfo method = typeof (StructAbiFast).GetMethod (
			name, BindingFlags.Static | BindingFlags.Public);

		if (method == null) {
			Console.WriteLine ("FAIL: no StructAbiFast.{0}", name);
			return false;
		}

		IntPtr handle = method.MethodHandle.Value;

		Mono.Tiering.MonoTier.PromoteNow (handle, tier1);

		for (int i = 0; i < 200; i++) {
			if (Mono.Tiering.MonoTier.GetTier (handle) >= tier1)
				return true;
			Thread.Sleep (10);
		}

		Console.WriteLine ("FAIL: StructAbiFast.{0} never reached tier 1", name);
		return false;
	}

	// Reflection is what asks the backend for a method, and asking is what
	// gives the classic filter a chance to answer for it. An ordinary call
	// from interpreted code never asks: the interpreter reaches a callee it
	// can interpret itself.
	static void enter_classic (string name)
	{
		MethodInfo method = typeof (StructAbiClassic).GetMethod (
			name, BindingFlags.Static | BindingFlags.Public);

		method.Invoke (null, null);
	}

	public static int Main ()
	{
		bool promoted = true;

		foreach (string name in driven)
			promoted &= promote (name);

		if (!promoted)
			return 1;

		// Runs Warm () before the sweeps below, so every Take/Make method it
		// calls already has a classic body: an interpreted caller reaches
		// them through the jit-call path instead of interpreting them too.
		enter_classic ("Warm");

		StructAbiCheck.SweepClassic ();
		StructAbiCheck.SweepFast ();
		StructAbiCheck.SweepInterp ();
		StructAbiCheck.Delegates ("interp -> invoke wrapper ");

		enter_classic ("DriveFast");
		enter_classic ("DriveInterp");
		enter_classic ("DriveDelegates");

		StructAbiFast.DriveClassic ();

		if (StructAbiCheck.Failures != 0) {
			Console.WriteLine ("FAIL: {0} boundaries disagreed",
			                   StructAbiCheck.Failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
