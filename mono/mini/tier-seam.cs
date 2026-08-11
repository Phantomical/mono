using System;
using System.Numerics;
using System.Runtime.CompilerServices;

/*
 * Calls that cross from an interpreted caller into a compiled callee, which is
 * the shape do_jit_call () marshals through the gsharedvt_out_sig wrapper.
 *
 * Each test loops far enough for its callee to spend its tier-0 call counter
 * and be compiled underneath it, while the test method itself is entered once
 * by the harness and so stays interpreted for the whole run. The loop count is
 * not decoration: the promotion is a background compile, and the crossing has
 * been seen as late as iteration 250000 on a loaded machine. The harness
 * additionally requires each callee to appear in the JIT trace, so a run where
 * the compile never landed fails rather than passing without having tested
 * anything.
 *
 * The returns are the interesting axis. A value type too wide for the return
 * registers comes back through a hidden pointer, and where that pointer sits
 * among the arguments is derived from the prototype at both ends -- so a
 * wrapper that describes the callee with a different argument count than the
 * callee has puts the pointer in a register the callee does not read.
 */

struct Wide {
	public long a, b, c, d;
}

struct Narrow {
	public long a, b;
}

/* Field-for-field what Vector4 is, and deliberately not a SIMD type: the two
 * travel in different registers, so this is the control that says whether a
 * failure below is about the crossing or merely about sixteen bytes. */
struct Quad {
	public float a, b, c, d;
}

class Tests
{
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (Tests), args);
	}

	const int iterations = 4000000;

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Wide wide_static_noargs () {
		Wide w;
		w.a = 1; w.b = 2; w.c = 3; w.d = 4;
		return w;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Wide wide_static_onearg (long seed) {
		Wide w;
		w.a = seed; w.b = seed + 1; w.c = seed + 2; w.d = seed + 3;
		return w;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	Wide wide_instance_noargs () {
		Wide w;
		w.a = 5; w.b = 6; w.c = 7; w.d = 8;
		return w;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Narrow narrow_static_noargs () {
		Narrow n;
		n.a = 11; n.b = 22;
		return n;
	}

	/* A hidden return pointer and no other argument: the callee's prototype has
	 * one argument, and anything describing it with two puts the pointer in the
	 * second register while the callee reads the first. */
	public static int test_0_wide_static_noargs () {
		for (int i = 0; i < iterations; i++) {
			Wide w = wide_static_noargs ();
			if (w.a != 1 || w.b != 2 || w.c != 3 || w.d != 4)
				return 1;
		}
		return 0;
	}

	public static int test_0_wide_static_onearg () {
		for (int i = 0; i < iterations; i++) {
			Wide w = wide_static_onearg (i);
			if (w.a != i || w.b != i + 1 || w.c != i + 2 || w.d != i + 3)
				return 1;
		}
		return 0;
	}

	public static int test_0_wide_instance_noargs () {
		Tests self = new Tests ();

		for (int i = 0; i < iterations; i++) {
			Wide w = self.wide_instance_noargs ();
			if (w.a != 5 || w.b != 6 || w.c != 7 || w.d != 8)
				return 1;
		}
		return 0;
	}

	/* Narrow enough to come back in registers, so there is no hidden pointer to
	 * misplace. Its failure mode is a wrong value rather than a fault, which is
	 * why the fields are checked rather than just the call returning. */
	public static int test_0_narrow_static_noargs () {
		for (int i = 0; i < iterations; i++) {
			Narrow n = narrow_static_noargs ();
			if (n.a != 11 || n.b != 22)
				return 1;
		}
		return 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Vector4 simd_roundtrip (Vector4 v) {
		return v;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static Quad quad_roundtrip (Quad q) {
		return q;
	}

	/*
	 * A SIMD class travels in one vector register while a struct of the same
	 * fields travels in two, so the wrapper the crossing is marshalled through
	 * cannot be shared between them: sharing describes the call one way and
	 * compiles the callee the other, and the callee reads half a value.
	 */
	public static int test_0_simd_roundtrip () {
		Vector4 v = new Vector4 (1.0f, 2.0f, 3.0f, 4.0f);

		for (int i = 0; i < iterations; i++) {
			Vector4 r = simd_roundtrip (v);
			if (r.X != 1.0f || r.Y != 2.0f || r.Z != 3.0f || r.W != 4.0f)
				return 1;
		}
		return 0;
	}

	/* The control for the test above: same fields, same width, no vector
	 * register. It failing too would mean the crossing is broken for every
	 * sixteen-byte value rather than for SIMD ones. */
	public static int test_0_quad_roundtrip () {
		Quad q = new Quad ();
		q.a = 1.0f; q.b = 2.0f; q.c = 3.0f; q.d = 4.0f;

		for (int i = 0; i < iterations; i++) {
			Quad r = quad_roundtrip (q);
			if (r.a != 1.0f || r.b != 2.0f || r.c != 3.0f || r.d != 4.0f)
				return 1;
		}
		return 0;
	}
}
