using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

using MonoTests.Inliner;

//
// Differential test corpus for the tier-1 top-down LLVM inliner
// (mono/mini/llvm/inliner.cpp).
//
// Every helper below that is meant to be a CANDIDATE for the inliner is called
// from a small [MethodImpl(NoInlining)] "hot" wrapper, looped enough times to
// cross MONO_TIERED_CALL_THRESHOLD at every threshold tieredcheck exercises.
// Once the wrapper promotes to tier 1, it is itself the LLVM module's root
// (see translator.cpp's "mono-tier1-root" comment), and its calls to the
// helper are exactly what the top-down inliner pass looks at.
//
// The corpus is differential BY CONSTRUCTION: every test_N method's expected
// value N (or 0, by the shared repo convention - see TestDriver.cs) is the
// classic/tier-0 answer, computed the same way regardless of which tier ran
// it. Running under `MONO_TIERED=1 --llvm` exercises the inliner; running
// under classic tier 0 (no --llvm) exercises none of it. Both must return the
// same, correct value - if a wrong inlining decision changes observable
// behaviour, only the tier-1 run goes wrong and the test fails there.
//
// The tier-1 run that actually inlines is the STANDALONE one (Main below, via
// TestDriver), not `--regression`. Under --regression the first opt-set is 0,
// mini_regression_step () installs it with mono_set_defaults (), and tier 1
// latches a method's opt the first time it promotes - so every root spends the
// rest of the process at -O=-inline and the pass stands down on all of them
// (visible as "refuse-root-no-opt-inline" under MONO_INLINER_TRACE=1). Both
// runs are in tieredcheck; only the standalone one is inliner coverage.
//
// A test that works out its expected value by recomputing the callee's logic
// inline carries [MethodImpl(NoOptimization)], which keeps it on the classic
// JIT (mono_llvm_check_method_supported declines it). Without that the test
// method promotes as well - it is entered once per opt combination and the
// tier-0 prologue's call counter takes it over any threshold >= 1 - and the
// reference arithmetic ends up compiled by the very backend it is meant to be
// checking, so the comparison stops being differential. The method under test
// is never the test method itself: it is always the [MethodImpl(NoInlining)]
// hot wrapper the test drives, which still promotes normally. Tests comparing
// against a plain literal need no attribute, since a constant cannot be
// co-miscompiled.
//
// A note on why some leaf helpers below look padded with unused arithmetic:
// classic mini has its own, much older front-end inliner (method-to-ir.c,
// inline_method()) that runs regardless of backend, and for a method compiled
// for the LLVM tier it will happily fold in any callee under ~100 IL bytes
// before the LLVM translator ever sees a call site. That would fold the
// candidate into the caller's IR before OUR pass ever ran, so the test would
// stop being an exercise of this inliner at all. Padding a callee's IL past
// that threshold (with a chain of local computations classic's inliner never
// looks past for the callee's ACTUAL logic) keeps the call site intact for
// the top-down pass. Where a callee is already guaranteed to survive - it has
// exception clauses, is NoInlining/Synchronized, or already makes non-leaf
// calls - no padding is needed, since classic mini declines it outright too.
//
namespace MonoTests.Tiering {
	static class Probe {
		// MONO_TIERED_CALL_THRESHOLD, or 0 when deferred promotion is off.
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

public class InlinerTests {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (InlinerTests), args);
	}

	// How many times to enter a helper so that it is promoted well before the
	// loop ends. Sized from the configured threshold rather than fixed at some
	// number that clears the largest one: --regression replays every test in
	// here once per opt set, and these loops are most of what that costs.
	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

	// ==========================================================================
	// POSITIVE: leaf calls the inliner SHOULD fold, and fold correctly.
	// ==========================================================================

	static int leaf_square_plus_calls;

	// Padded well past the 100-IL-byte LLVM inline limit so classic mini leaves
	// the call to HotLeafCaller alone; the top-down pass is what has to decide
	// on it. The side-effect counter catches a double-run or dropped-run bug
	// that pure arithmetic might accidentally paper over.
	//
	// The padding is deliberately add/sub/mul/shift/bitwise only - no `/` or
	// `%`. Those IL opcodes always carry a divide-by-zero (and, for signed
	// division, a MinValue/-1 overflow) guard, which the front end emits as an
	// actual call to the corlib-exception trampoline even when the divisor is
	// a provably-safe constant; that call only becomes dead, unreachable code
	// once the per-candidate simplification pass runs, which happens AFTER
	// is_leaf_body() looks at the raw materialized body. A leaf candidate that
	// happens to divide would therefore read as non-leaf for a reason that has
	// nothing to do with the leaf/non-leaf gate this corpus means to exercise.
	static int LeafSquarePlus (int x) {
		leaf_square_plus_calls++;
		int p1 = x + 1, p2 = p1 * 3, p3 = p2 - 7, p4 = p3 ^ 11, p5 = p4 & 0xFF;
		int p6 = p5 | 0x20, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;			// unreachable for the x range this is called with
		return x * x + 3;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int HotLeafCaller (int x) {
		return LeafSquarePlus (x) + 1;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_leaf_inline_correct () {
		leaf_square_plus_calls = 0;
		long sum = 0;
		const int CYCLE = 50;
		int ITERS = (Iters () + CYCLE - 1) / CYCLE * CYCLE;
		for (int i = 0; i < ITERS; i++)
			sum += HotLeafCaller (i % CYCLE);
		long perCycle = 0;
		for (int k = 0; k < CYCLE; k++)
			perCycle += k * k + 3 + 1;
		long expected = perCycle * (ITERS / CYCLE);
		if (sum != expected)
			return 1;
		if (leaf_square_plus_calls != ITERS)
			return 2;			// folded body ran a different number of times
						// than it was called - exactly-once semantics broke.
		return 0;
	}

	// Three independent leaf calls composed at the root, each individually
	// eligible: proves the pass handles more than one candidate per root and
	// that their results compose correctly once folded together.
	static int LeafChainA (int x) {
		int p1 = x + 2, p2 = p1 * 5, p3 = p2 - 9, p4 = p3 ^ 3, p5 = p4 & 0xFF;
		int p6 = p5 | 0x40, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 2) + 1;
		int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x + 1;
	}

	static int LeafChainB (int x) {
		int p1 = x + 3, p2 = p1 * 7, p3 = p2 - 4, p4 = p3 ^ 6, p5 = p4 & 0xFF;
		int p6 = p5 | 0x08, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 3) + 1;
		int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x * 2;
	}

	static int LeafChainC (int x) {
		int p1 = x + 5, p2 = p1 * 2, p3 = p2 - 6, p4 = p3 ^ 9, p5 = p4 & 0xFF;
		int p6 = p5 | 0x02, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x - 3;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ChainedHotCaller (int x) {
		return LeafChainC (LeafChainB (LeafChainA (x)));
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_chained_leaf_inline () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ChainedHotCaller (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = i % 100;
			expected += ((x + 1) * 2) - 3;
		}
		return sum == expected ? 0 : 1;
	}

	// The caller branches on the leaf's return value, so a wrong fold that
	// e.g. swapped an argument or dropped a term would send execution down the
	// wrong branch, not just compute a wrong number in a straight line. Uses
	// `& 7` rather than `% 7` for the same reason the padding avoids `/` and
	// `%` above - a real division/modulo in the candidate's own logic would
	// trip the leaf gate on its pre-simplification overflow-check call too.
	static int LeafCompute (int x) {
		int p1 = x + 4, p2 = p1 * 3, p3 = p2 - 2, p4 = p3 ^ 8, p5 = p4 & 0xFF;
		int p6 = p5 | 0x01, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return (x * x) & 7;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int BranchHotCaller (int x) {
		int v = LeafCompute (x);
		return v > 3 ? v * 10 : v + 1;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_leaf_computed_value_branch () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += BranchHotCaller (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int v = ((i % 100) * (i % 100)) & 7;
			expected += v > 3 ? v * 10 : v + 1;
		}
		return sum == expected ? 0 : 1;
	}

	// ==========================================================================
	// GENERICS / GSHARED / RGCTX.
	//
	// Mono JIT-compiles a generic method or type instantiated over a REFERENCE
	// type once, in shared ("gshared") form, and threads a runtime generic
	// context (rgctx) into calls that need it to recover the real type. The
	// tier-1 inliner does not fold that shared body in: materialize_callee ()
	// compiles the callee's EXACT instantiation instead, which needs no rgctx at
	// all. So a reference-type instantiation inlines on the same terms as a value
	// type one, and both are judged on the ordinary gates.
	//
	// A call site may still pass a vtable/mrgctx in the `nest` argument - the
	// caller decided that from the callee's metadata, long before. The
	// specialized body keeps the parameter (so the types still match) and ignores
	// it.
	//
	// What is still refused is a callee that is ALREADY shared - a `T_REF` symbol,
	// which only a gshared root can reach. There is no exact instantiation to
	// compile there, so those keep their trampoline call. Covered below by
	// Mix<T_REF> -> Box<T_REF>.
	// ==========================================================================

	// Same leaf, instantiated once over a value type (int - never shared) and once
	// over a reference type (string - shared as `T_REF` when mono compiles it for
	// real, specialized when the inliner materializes it).
	// The padding int-only local chain keeps this above the classic inline
	// limit without needing arithmetic on the (unconstrained) T itself.
	static T GenericPick<T> (T a, T b, bool pickFirst, int pad) {
		int p1 = pad + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
		int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p3, p12 = p11 - p4;
		if (p12 == int.MinValue)
			return b;			// unreachable for the pad range used below
		return pickFirst ? a : b;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenericPickHotInt (int x) {
		return GenericPick<int> (x, x + 1000, (x & 1) == 0, x);
	}

	// INLINER-EXPECT: folded InlinerTests:GenericPick<int> (int,int,bool,int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_leaf_valuetype_inlines () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += GenericPickHotInt (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = i % 100;
			expected += (x & 1) == 0 ? x : x + 1000;
		}
		return sum == expected ? 0 : 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenericPickHotString (int x) {
		string a = "a" + x, b = "bb" + x;
		string r = GenericPick<string> (a, b, (x & 1) == 0, x);
		return r.Length;
	}

	// The reference-type twin of the test above. GenericPick<string> is what mono
	// compiles shared; the inliner materializes a specialized GenericPick<string>
	// and folds that in instead ("expose", and GenericPick<T_REF> stops being
	// promoted at all because nothing calls the shared body any more).
	// INLINER-EXPECT: folded InlinerTests:GenericPick<string> (string,string,bool,int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_leaf_reftype_inlines () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += GenericPickHotString (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = i % 100;
			string a = "a" + x, b = "bb" + x;
			expected += ((x & 1) == 0 ? a : b).Length;
		}
		return sum == expected ? 0 : 1;
	}

	// A STATIC method on a generic class - the shape that carries a `nest`
	// argument. check_method_sharing () decides from metadata that a sharable
	// static generic callee is passed a vtable, and that decision was made when
	// the caller's front-end ran, so the call site has the extra argument whether
	// or not the body ends up wanting it. The specialized body keeps the
	// parameter and ignores it; if it dropped it, the body and the declaration
	// would have different LLVM types and the call site could not be rewired at
	// all (the pass asserts they match). The int instantiation is not sharable,
	// so it is passed nothing - the two together cover both paths.
	class StaticGenericHolder<T> {
		public static int Weigh (T a, T b, bool first, int pad) {
			int p1 = pad + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
			int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
			int p11 = p10 + p3, p12 = p11 - p4;
			if (p12 == int.MinValue)
				return 0;			// unreachable for the pad range used below
			return (first ? a : b).ToString ().Length;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int StaticGenericHotString (int x) {
		string a = "a" + x, b = "bb" + x;
		return StaticGenericHolder<string>.Weigh (a, b, (x & 1) == 0, x);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int StaticGenericHotInt (int x) {
		return StaticGenericHolder<int>.Weigh (x, x + 1000, (x & 1) == 0, x);
	}

	// INLINER-EXPECT: folded InlinerTests/StaticGenericHolder`1<string>:Weigh (string,string,bool,int)
	// INLINER-EXPECT: folded InlinerTests/StaticGenericHolder`1<int>:Weigh (int,int,bool,int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_static_generic_class_method_inlines () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += StaticGenericHotString (i % 100) + StaticGenericHotInt (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = i % 100;
			string a = "a" + x, b = "bb" + x;
			expected += ((x & 1) == 0 ? a : b).Length;
			expected += ((x & 1) == 0 ? x : x + 1000).ToString ().Length;
		}
		return sum == expected ? 0 : 1;
	}

	// A generic method that constructs another generic type (Mix<T> -> new
	// Box<T>(a)). Both instantiations materialize as specialized bodies. This is
	// also where the remaining refusal shows up: promoting Mix<T_REF> itself makes
	// a gshared ROOT, and the Box<T_REF>:.ctor it calls is an already-shared
	// callee with no exact instantiation to compile, so that one stays on its
	// trampoline. The
	// padding int parameter (unused by the real logic beyond an unreachable
	// guard) exists for the same reason as the leaf padding above: without it
	// Mix<T>'s IL body is small enough that CLASSIC mini's own inliner folds
	// the whole call into its caller before the top-down pass ever sees it -
	// which would make this pass at tier 1 for the wrong reason (nothing left
	// to refuse) rather than by actually exercising the gate.
	class Box<T> {
		public T Value;
		public Box (T v) { Value = v; }
	}

	static T Mix<T> (T a, int pad) {
		int p1 = pad + 1, p2 = p1 * 3, p3 = p2 - 7, p4 = p3 ^ 11, p5 = p4 & 0xFF;
		int p6 = p5 | 0x20, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return a;			// unreachable for the pad range used below
		var b = new Box<T> (a);
		return b.Value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int MixHotInt (int x) {
		return Mix<int> (x, x);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int MixHotString (int x) {
		return Mix<string> ("v" + x, x).Length;
	}

	// INLINER-EXPECT: refused InlinerTests/Box`1<T_REF>:.ctor (T_REF)
	// INLINER-EXPECT: folded InlinerTests:Mix<string> (string,int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_constructs_generic_type () {
		long sumInt = 0, sumStr = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++) {
			sumInt += MixHotInt (i);
			sumStr += MixHotString (i % 1000);
		}
		long expectedInt = 0, expectedStr = 0;
		for (int i = 0; i < ITERS; i++) {
			expectedInt += i;
			expectedStr += ("v" + (i % 1000)).Length;
		}
		if (sumInt != expectedInt)
			return 1;
		if (sumStr != expectedStr)
			return 2;
		return 0;
	}

	// The ROOT itself is gshared: GsharedCallsGshared<string> is a generic
	// method instantiated over a reference type, so when IT is the method
	// promoted to tier 1 (called directly and repeatedly below, not through a
	// non-generic wrapper), its own compile has cfg->gshared set and
	// tier1_root_allows_inlining() refuses to inline ANYTHING out of it -
	// the calls to Identity/Mix below are never even scanned. Nothing to
	// trace; what matters is the result stays correct with inlining
	// structurally off for this whole root.
	static T Identity<T> (T a) { return a; }

	static T GsharedCallsGshared<T> (T a) {
		return Identity (Mix (a, 0));
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_gshared_root_declines_inlining () {
		string last = null;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			last = GsharedCallsGshared<string> ("g" + (i % 7));
		string expected = "g" + ((ITERS - 1) % 7);
		return last == expected ? 0 : 1;
	}

	// A gshared ROOT calling a callee shared over exactly the type parameters the
	// root is shared over: same class, same instantiation, so the two share one
	// runtime generic context and the call site already passes it. That callee is
	// materialized as the shared body it is rather than refused.
	//
	// The two instantiations below run the SAME shared code, which is what makes
	// this differential: `new T[2]` reads the element vtable out of the rgctx, so
	// a folded-in body that lost the context - or kept one instantiation's - would
	// build an array of the wrong element type, and the store into it would throw.
	class SharedRing<T> where T : class {
		public T Value;
		public SharedRing (T v) { Value = v; }

		public T[] Take (int pad) {
			int p1 = pad + 1, p2 = p1 * 3, p3 = p2 - 7, p4 = p3 ^ 11, p5 = p4 & 0xFF;
			int p6 = p5 | 0x20, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
			int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
			if (p15 == int.MinValue)
				return null;		// unreachable for the pad range used below
			T[] a = new T [2];
			a [0] = Value;
			a [1] = Value;
			return a;
		}

		[MethodImpl (MethodImplOptions.NoInlining)]
		public int Hot (int x) {
			T[] a = Take (x);
			return a.Length + (a.GetType () == typeof (T[]) ? 1 : 0);
		}
	}

	// INLINER-EXPECT: folded InlinerTests/SharedRing`1<T_REF>:Take (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_gshared_callee_shares_root_context () {
		var s = new SharedRing<string> ("v");
		var o = new SharedRing<object> (new object ());
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += s.Hot (i) + o.Hot (i);
		return sum == 6L * ITERS ? 0 : 1;
	}

	// Dictionary<T, List<T>>-ish nested generic container. Deeply non-leaf
	// (Dictionary/List method calls), and for a reference-type T every one of
	// those callees is a shared instantiation the inliner now specializes;
	// either way, must stay correct.
	static int NestedGenericsContainer<T> (T a, T b) {
		var dict = new Dictionary<T, List<T>> ();
		var list = new List<T> ();
		list.Add (a);
		list.Add (b);
		dict [a] = list;
		return dict [a].Count;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int NestedGenericsHotInt (int x) {
		return NestedGenericsContainer<int> (x, x + 1);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int NestedGenericsHotString (int x) {
		return NestedGenericsContainer<string> ("a" + x, "b" + x);
	}

	public static int test_0_nested_generics_container () {
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++) {
			if (NestedGenericsHotInt (i) != 2)
				return 1;
			if (NestedGenericsHotString (i) != 2)
				return 2;
		}
		return 0;
	}

	// A small generic container helper (Pair<T>.Swap), instantiated over a
	// value type. The ctor + Swap() calls make it non-leaf, so it is refused
	// regardless of genericity - included because the review explicitly asked
	// for "a generic container helper" as its own case. Swap()'s padding is a
	// fixed-seed int chain (T is unconstrained, so no arithmetic on First/
	// Second is available to pad with) - same classic-mini-inline-limit
	// reasoning as Mix<T> above.
	class Pair<T> {
		public T First, Second;
		public Pair (T a, T b) { First = a; Second = b; }

		public void Swap () {
			int p1 = 5, p2 = p1 * 3, p3 = p2 - 7, p4 = p3 ^ 11, p5 = p4 & 0xFF;
			int p6 = p5 | 0x20, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
			int p11 = p10 + p4, p12 = p11 - p5, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
			if (p15 == int.MinValue)
				return;			// unreachable - the seed above is fixed
			T t = First;
			First = Second;
			Second = t;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenericContainerHelper (int a, int b) {
		var p = new Pair<int> (a, b);
		p.Swap ();
		return p.First * 1000 + p.Second;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_container_helper () {
		int ITERS = Iters ();
		long sum = 0;
		for (int i = 0; i < ITERS; i++)
			sum += GenericContainerHelper (i, i + 1);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += (i + 1) * 1000 + i;
		return sum == expected ? 0 : 1;
	}

	// ==========================================================================
	// CCTOR BARRIER. Each callee below is an otherwise-eligible leaf (small, no
	// clauses, not NoInlining) that reads a static field guarded by a real class
	// cctor. Padded to survive classic mini's own inliner so the call reaches the
	// top-down pass. All three holders use an explicit static ctor, so the
	// front-end leaves a barrier in the callee's own body and folding it in
	// carries that barrier along - the value each test checks is what a missing
	// or mis-hoisted one would corrupt.
	// ==========================================================================

	static class SeedHolder {
		public static int Seed;
		static SeedHolder () { Seed = 424242; }
	}

	static int ReadSeed (int x) {
		int p1 = x + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
		int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 / 3 + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x + SeedHolder.Seed;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadSeedHotCaller (int x) {
		return ReadSeed (x);
	}

	// INLINER-EXPECT: folded InlinerTests:ReadSeed (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_field_read () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReadSeedHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 424242;
		return sum == expected ? 0 : 1;
	}

	static class CctorProp {
		static int _val;
		static CctorProp () { _val = 777; }
		public static int Val {
			get {
				int p1 = _val + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
				int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 / 3 + 1;
				int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7;
				if (p14 == int.MinValue)
					return -1;
				return _val;
			}
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadCctorPropHotCaller (int x) {
		return x + CctorProp.Val;
	}

	// INLINER-EXPECT: folded InlinerTests/CctorProp:get_Val ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_property_getter () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReadCctorPropHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 777;
		return sum == expected ? 0 : 1;
	}

	static class ReadonlyHolder {
		public static readonly int Value;
		static ReadonlyHolder () { Value = 555; }
	}

	static int ReadReadonly (int x) {
		int p1 = x + 2, p2 = p1 * 3, p3 = p2 - 4, p4 = p3 ^ 6, p5 = p4 & 0xFF;
		int p6 = p5 | 0x08, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 / 3 + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x + ReadonlyHolder.Value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadReadonlyHotCaller (int x) {
		return ReadReadonly (x);
	}

	// INLINER-EXPECT: folded InlinerTests:ReadReadonly (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_readonly_field () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReadReadonlyHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 555;
		return sum == expected ? 0 : 1;
	}

	// "Marvin-seed style" real-world sanity check: Dictionary<string,int> hashes
	// its keys through corlib's own randomized-seed machinery, a cctor-guarded
	// static this test does not control the source of. This is not aimed at any
	// one refuse-* trace tag - Dictionary's own methods are generic/non-leaf and
	// get excluded by other gates well before the cctor check matters - it is a
	// broader round-trip correctness net for code that transitively touches
	// cctor-guarded corlib state under tier 1.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int DictRoundtrip (string key, int val) {
		var d = new Dictionary<string, int> ();
		d [key] = val;
		int result;
		return d.TryGetValue (key, out result) ? result : -1;
	}

	public static int test_0_cctor_dictionary_roundtrip () {
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++) {
			string key = "key" + i;
			if (DictRoundtrip (key, i * 7) != i * 7)
				return 1;
		}
		return 0;
	}

	// ==========================================================================
	// CLASS-INIT TRIGGER. The mirror image of the section above: there the callee
	// read cctor-guarded static state, here it touches no static state at all, so
	// nothing about its body is suspect. What is at stake is that the CALL is the
	// class-init trigger for the callee's own class - the first one lands in a
	// trampoline, which compiles the method and then runs its class's cctor. Fold
	// the callee in and the only surviving trigger is the class-init preamble the
	// tier-1 body carries; if that is missing, the cctor never runs at all for the
	// whole process, and the failure surfaces at an arbitrary static read far away.
	//
	// Only bites when the caller is promoted before the callee's class has been
	// initialized, i.e. at MONO_TIERED_CALL_THRESHOLD=0, where a method is
	// promoted right after its tier-0 compile and before it has ever run.
	// ==========================================================================

	static class TriggerObserver {
		public static int Ran;
	}

	static class TriggerHolder {
		static TriggerHolder () { TriggerObserver.Ran = 1234; }

		// Deliberately reads nothing static, and does no division: the callee has
		// to be blameless to every other gate - there is no static access to
		// blame, and no idiv overflow check to make the body non-leaf - so the
		// class-init trigger is the only thing under test. No IL-size padding is
		// needed either: classic mini declines to inline a callee whose class is
		// not initialized yet, which is precisely the situation under test, so
		// the call always survives to the top-down pass.
		public static int Compute (int x) {
			int p1 = x + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
			int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 + 1;
			int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
			if (p15 == int.MinValue)
				return -1;
			return x + 9;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ComputeHotCaller (int x) {
		return TriggerHolder.Compute (x);
	}

	// INLINER-EXPECT: folded InlinerTests/TriggerHolder:Compute (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_class_init_trigger () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ComputeHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 9;
		if (sum != expected)
			return 1;
		// The cctor is the whole point: if the call got inlined away, nothing
		// ever ran it and TriggerObserver.Ran is still 0.
		return TriggerObserver.Ran == 1234 ? 0 : 2;
	}

	// The same trigger, on a GENERIC class. Worth its own fixture because this
	// shape used to be unreachable: pending_class_init_vtable () gives up
	// ("indeterminate") on a class whose vtable it cannot pin down, and before the
	// callee was compiled specialized there was no single vtable to name. A closed
	// instantiation has exactly one, so the preamble can trigger it like any other.
	static class GenericTriggerObserver {
		public static int Ran;
	}

	static class GenericTriggerHolder<T> {
		static GenericTriggerHolder () { GenericTriggerObserver.Ran += 1; }

		// Same rules as TriggerHolder.Compute above: blameless to every other gate,
		// so the class-init preamble is the only thing under test.
		public static int Compute (int x) {
			int p1 = x + 1, p2 = p1 * 2, p3 = p2 - 3, p4 = p3 ^ 5, p5 = p4 & 0xFF;
			int p6 = p5 | 0x10, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = p9 + 1;
			int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
			if (p15 == int.MinValue)
				return -1;
			return x + 11;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenericComputeHotCaller (int x) {
		// Two instantiations, so two distinct classes each owing a cctor: the
		// reference-type one is what mono would share, the value-type one it would
		// not, and both must end up initialized exactly once.
		return GenericTriggerHolder<string>.Compute (x) + GenericTriggerHolder<int>.Compute (x);
	}

	// INLINER-EXPECT: folded InlinerTests/GenericTriggerHolder`1<string>:Compute (int)
	// INLINER-EXPECT: folded InlinerTests/GenericTriggerHolder`1<int>:Compute (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_cctor_class_init_trigger () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += GenericComputeHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += (i + 11) * 2;
		if (sum != expected)
			return 1;
		// One cctor run per instantiation, and no more: a preamble that re-ran it
		// would show up here just as loudly as one that never ran it.
		return GenericTriggerObserver.Ran == 2 ? 0 : 2;
	}

	// ==========================================================================
	// ELIDED FOREIGN BARRIER, the shape with nothing in the IR to notice.
	// LazyHolder has no explicit static ctor, so the C# compiler marks it
	// beforefieldinit, and the front-end emits no barrier for it inside ReadLazy
	// at all - it leaves that cctor to the SFLDA patch resolution, which a tier-1
	// compile skips (run_cctors = FALSE). The three fixtures above keep a barrier
	// in the body; here the guard has to come from the metadata scan
	// (collect_static_access_classes ()), as a class-init preamble on the
	// materialized body. Fold ReadLazy in without one and Value reads back as 0.
	// ==========================================================================

	static class LazyHolder {
		public static int Value = MakeValue ();
		static int MakeValue () { LazyObserver.Ran = 1; return 31337; }
	}

	static class LazyObserver {
		public static int Ran;
	}

	static int ReadLazy (int x) {
		int p1 = x + 4, p2 = p1 * 3, p3 = p2 - 8, p4 = p3 ^ 12, p5 = p4 & 0xFF;
		int p6 = p5 | 0x04, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x + LazyHolder.Value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadLazyHotCaller (int x) {
		return ReadLazy (x);
	}

	// INLINER-EXPECT: folded InlinerTests:ReadLazy (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_beforefieldinit_static () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReadLazyHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 31337;
		if (sum != expected)
			return 1;
		// A guard that never ran leaves the cctor unrun for the whole process,
		// which the value check above only catches because Value happens to be
		// non-zero. Say it directly as well.
		return LazyObserver.Ran == 1 ? 0 : 2;
	}

	// Same shape, two foreign classes in one callee: the preamble has to chain a
	// guard per class rather than emit one and stop.

	static class PairObserver {
		public static int Left, Right;
	}

	static class PairLeft {
		public static int Value = MakeValue ();
		static int MakeValue () { PairObserver.Left = 1; return 100; }
	}

	static class PairRight {
		public static int Value = MakeValue ();
		static int MakeValue () { PairObserver.Right = 1; return 7; }
	}

	static int ReadPair (int x) {
		int p1 = x + 3, p2 = p1 * 5, p3 = p2 - 7, p4 = p3 ^ 9, p5 = p4 & 0xFF;
		int p6 = p5 | 0x02, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return -1;
		return x + PairLeft.Value + PairRight.Value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadPairHotCaller (int x) {
		return ReadPair (x);
	}

	// INLINER-EXPECT: folded InlinerTests:ReadPair (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_two_beforefieldinit_statics () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReadPairHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 107;
		if (sum != expected)
			return 1;
		return (PairObserver.Left == 1 && PairObserver.Right == 1) ? 0 : 2;
	}

	// The barrier belongs where the access is, not in the callee's prologue.
	// ColdHolder is read only down a branch the loop below never takes, so its
	// cctor must never run - a preamble guard would run it on the first call and
	// ColdObserver would say so. Explicit static ctor on purpose: a
	// beforefieldinit class is initialized eagerly when the accessing method is
	// JITted, which is tier-dependent and would make this untestable.

	static class ColdObserver {
		public static int Ran;
	}

	static class ColdHolder {
		public static int Value;
		static ColdHolder () { ColdObserver.Ran = 1; Value = 99; }
	}

	static int ReadCold (int x) {
		int p1 = x + 6, p2 = p1 * 7, p3 = p2 - 2, p4 = p3 ^ 3, p5 = p4 & 0xFF;
		int p6 = p5 | 0x20, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
		int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
		if (p15 == int.MinValue)
			return ColdHolder.Value;
		return x + 1;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReadColdHotCaller (int x) {
		return ReadCold (x);
	}

	// INLINER-EXPECT: folded InlinerTests:ReadCold (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_barrier_stays_on_its_path () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReadColdHotCaller (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + 1;
		if (sum != expected)
			return 1;
		return ColdObserver.Ran == 0 ? 0 : 2;
	}

	// ==========================================================================
	// FAILING CCTOR. ThrowingHolder's cctor throws, so its vtable never reaches
	// initialized and Compute's class-init preamble runs on every single call. The
	// exception has to come out of Compute and be caught by the caller's handler,
	// which is where the call site is - both when Compute keeps its call and when
	// it gets folded in, where the preamble's call has to be rewritten into an
	// invoke on the caller's landing pad.
	// ==========================================================================

	static class ThrowingHolder {
		static ThrowingHolder () { throw new InvalidOperationException ("boom"); }

		public static int Compute (int x) {
			int p1 = x + 7, p2 = p1 * 3, p3 = p2 - 2, p4 = p3 ^ 9, p5 = p4 & 0xFF;
			int p6 = p5 | 0x01, p7 = p6 + p1, p8 = p7 - p2, p9 = p8 * 2, p10 = (p9 >> 1) + 1;
			int p11 = p10 + p3, p12 = p11 - p4, p13 = p12 ^ p6, p14 = p13 + p7, p15 = p14 - p8;
			if (p15 == int.MinValue)
				return -1;
			return x + 2;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ThrowingCctorHotCaller (int x) {
		try {
			return ThrowingHolder.Compute (x);
		} catch (TypeInitializationException) {
			return -1;
		}
	}

	public static int test_0_cctor_failure_caught_by_caller () {
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			if (ThrowingCctorHotCaller (i) != -1)
				return 1;
		return 0;
	}

	// ==========================================================================
	// EH CLAUSE-BEARING CALLEES - refuse-eh (and, for the one shape custom-emit
	// EH cannot handle at all, a front-end decline with no trace). None of
	// these need IL-size padding: mono_method_check_inlining() in classic mini
	// declines any method with clauses outright, regardless of size, so the
	// call always survives to the top-down pass.
	// ==========================================================================

	static int LeafTryCatch (int x) {
		try {
			if (x < 0)
				throw new ArgumentOutOfRangeException ();
			return x + 100;
		} catch (ArgumentOutOfRangeException) {
			return -x;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int TryCatchHotCaller (int x) {
		return LeafTryCatch (x);
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_try_catch_leaf_refused () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++) {
			int x = (i % 3 == 0) ? -(i % 50) : (i % 50);
			sum += TryCatchHotCaller (x);
		}
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = (i % 3 == 0) ? -(i % 50) : (i % 50);
			expected += x < 0 ? -x : x + 100;
		}
		return sum == expected ? 0 : 1;
	}

	static int finally_run_count;

	static int LeafTryFinally (int x) {
		try {
			if (x < 0)
				throw new ArgumentException ();
			return x * 3;
		} finally {
			finally_run_count++;
		}
	}

	// The fixture for the call-site half of the EH rule. TryFinallyHotCaller
	// needs its own try/catch to swallow LeafTryFinally's exception, so the
	// call compiles to an `invoke` - it has an unwind edge to this frame's own
	// handler. Folding a clause-bearing body into a site like that would nest
	// the two methods' clauses, and the pads the callee brings carry selector
	// switches that know nothing of the caller's clauses, so the site keeps its
	// trampoline call. Since this is LeafTryFinally's only caller, nothing ends
	// up naming the materialized body and it is dropped again.
	// INLINER-EXPECT: refused InlinerTests:LeafTryFinally (int)
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int TryFinallyHotCaller (int x) {
		try {
			return LeafTryFinally (x);
		} catch (ArgumentException) {
			return -1;
		}
	}

	// The expected value IS the finally-run count: every call runs the finally
	// exactly once, whether it returns normally or throws through it, so after
	// ITERS calls the count must be exactly ITERS. A dropped finally (the
	// exact silent-miscompile this gate exists to prevent) would surface here
	// as a wrong, smaller number rather than a boolean pass/fail.
	public static int test_5000_try_finally_run_count_refused () {
		const int ITERS = 5000;
		finally_run_count = 0;
		int caught = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = (i % 5 == 0) ? -1 : (i % 100);
			int r = TryFinallyHotCaller (x);
			if (x >= 0) {
				if (r != x * 3)
					return -1;
			} else {
				if (r != -1)
					return -2;
				caught++;
			}
		}
		if (caught == 0)
			return -3;			// throwing branch never exercised - test is broken.
		return finally_run_count;
	}

	// try/fault (MonoTests.Inliner.FaultHelpers.TryFault, inliner-fault.il - C#
	// has no syntax for a bare fault clause). The fault handler must run
	// exactly on the exceptional path and never on the normal one. Like
	// LeafTryFinally above, TryFaultHotCaller's own try/catch turns this call
	// into an invoke, so it is excluded by the S0 invoke guard rather than
	// ever reaching the personality-function check.
	[MethodImpl (MethodImplOptions.NoInlining)]
	static int TryFaultHotCaller (int x) {
		try {
			return FaultHelpers.TryFault (x);
		} catch (InvalidOperationException) {
			return -1;
		}
	}

	public static int test_0_try_fault_leaf_refused () {
		FaultHelpers.FaultRunCount = 0;
		int ITERS = Iters ();
		int caught = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = (i % 4 == 0) ? -1 : (i % 50);
			int r = TryFaultHotCaller (x);
			if (x >= 0) {
				if (r != x * 2)
					return 1;
			} else {
				if (r != -1)
					return 2;
				caught++;
			}
		}
		if (caught == 0)
			return 3;
		if (FaultHelpers.FaultRunCount != caught)
			return 4;			// fault ran a different number of times than
						// the exceptional path was taken.
		return 0;
	}

	// A filter (`when`) clause: custom-emit EH's allowlist is {catch, finally,
	// fault} - a FILTER clause fails materialize_callee's own front-end
	// compile outright (translator.cpp's "filter clause" decline), so this
	// callee is never even offered to the leaf/EH gates; it just stays a
	// trampoline call. No trace line to look for, only correctness.
	static int LeafWithFilter (int x) {
		try {
			if (x == 0)
				throw new InvalidOperationException ("zero");
			return x * 2;
		} catch (InvalidOperationException ex) when (ex.Message == "zero") {
			return -1;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FilterHotCaller (int x) {
		return LeafWithFilter (x);
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_try_filter_when_refused () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += FilterHotCaller (i % 50);
		long expected = 0;
		for (int i = 0; i < ITERS; i++) {
			int x = i % 50;
			expected += x == 0 ? -1 : x * 2;
		}
		return sum == expected ? 0 : 1;
	}

	// Nested try/catch/finally, exercising three separate clauses (an outer
	// catch, an inner finally, an inner catch) in one method.
	static int LeafNestedHandlers (int x) {
		int steps = 0;
		try {
			try {
				if (x == 0)
					throw new InvalidOperationException ();
				steps += 1;
			} finally {
				steps += 10;
			}
			try {
				if (x < 0)
					throw new ArgumentException ();
				steps += 100;
			} catch (ArgumentException) {
				steps += 1000;
			}
		} catch (InvalidOperationException) {
			steps += 10000;
		}
		return steps;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int NestedHandlersHotCaller (int x) {
		return LeafNestedHandlers (x);
	}

	[MethodImpl (MethodImplOptions.NoOptimization | MethodImplOptions.NoInlining)]
	static int ExpectedNestedHandlers (int x) {
		int steps = 0;
		try {
			try {
				if (x == 0)
					throw new InvalidOperationException ();
				steps += 1;
			} finally {
				steps += 10;
			}
			try {
				if (x < 0)
					throw new ArgumentException ();
				steps += 100;
			} catch (ArgumentException) {
				steps += 1000;
			}
		} catch (InvalidOperationException) {
			steps += 10000;
		}
		return steps;
	}

	// The x==0 case unwinds an InvalidOperationException out of the inner try,
	// through the inner finally, to the outer catch - one throw needing both a
	// cleanup and a handler out of the same frame. A wrong answer here is
	// silent rather than a crash: the finally's +10 simply goes missing, so the
	// check counts mismatches against the oracle across the whole loop instead
	// of spot-checking one call.
	//
	// ExpectedNestedHandlers is the oracle and carries NoOptimization, which
	// keeps it on the classic JIT. Without that both sides can be compiled the
	// same way and agree on the same wrong answer - which is exactly how this
	// defect stayed hidden.
	public static int test_0_nested_handlers () {
		int errors = 0;
		int[] xs = { 0, -1, 5 };
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++) {
			int x = xs [i % xs.Length];
			int r = NestedHandlersHotCaller (x);
			if (r != ExpectedNestedHandlers (x))
				errors += 1;
		}
		return errors;
	}

	// ==========================================================================
	// REFLECTION-FRAME CALLEES - refuse-frame. Each of these calls into the
	// runtime's frame-walking machinery, which reports the frame of the method
	// making the call - so folding that method into its caller changes the
	// answer. StackTrace's frame-0 check is the most sensitive of the three:
	// if the callee's own call frame were ever folded away by a wrong inline,
	// frame 0 would resolve to the CALLER instead.
	// ==========================================================================

	static int LeafGetCurrentMethod () {
		var m = MethodBase.GetCurrentMethod ();
		return (m != null && m.Name == "LeafGetCurrentMethod") ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReflectCurrentMethodHotCaller () {
		return LeafGetCurrentMethod ();
	}

	public static int test_0_reflect_get_current_method () {
		int sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReflectCurrentMethodHotCaller ();
		return sum == ITERS ? 0 : 1;
	}

	static int LeafCallingAssembly () {
		var asm = Assembly.GetCallingAssembly ();
		return asm != null ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReflectCallingAssemblyHotCaller () {
		return LeafCallingAssembly ();
	}

	public static int test_0_reflect_calling_assembly () {
		int sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReflectCallingAssemblyHotCaller ();
		return sum == ITERS ? 0 : 1;
	}

	static int LeafStackTraceFrame () {
		var st = new StackTrace ();
		var frame = st.GetFrame (0);
		var m = frame != null ? frame.GetMethod () : null;
		return (m != null && m.Name == "LeafStackTraceFrame") ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ReflectStackTraceHotCaller () {
		return LeafStackTraceFrame ();
	}

	public static int test_0_reflect_stack_trace_frame () {
		int sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ReflectStackTraceHotCaller ();
		return sum == ITERS ? 0 : 1;
	}

	// ==========================================================================
	// SYNCHRONIZED under contention. materialize_callee() refuses a
	// METHOD_IMPL_ATTRIBUTE_SYNCHRONIZED callee unconditionally (mono-side, no
	// trace line) because its monitor enter/exit lives in the synchronized
	// wrapper, not the raw body. Several threads hammer the same counter; an
	// exact final count is only possible if every increment stayed serialized
	// by the lock, which a dropped monitor would very quickly break.
	// ==========================================================================

	static class SyncCounter {
		static int count;

		[MethodImpl (MethodImplOptions.Synchronized)]
		public static void Increment () {
			count++;
		}

		public static int Count { get { return count; } }

		// --regression re-runs every test_N in the same process, once per
		// optimization combination, so static state must be reset per call.
		public static void Reset () { count = 0; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static void SyncHotCaller () {
		SyncCounter.Increment ();
	}

	public static int test_0_synchronized_counter_contention () {
		SyncCounter.Reset ();
		const int NUM_THREADS = 8;
		const int ITERS = 5000;
		var threads = new Thread[NUM_THREADS];
		for (int t = 0; t < NUM_THREADS; t++) {
			threads [t] = new Thread (() => {
				for (int i = 0; i < ITERS; i++)
					SyncHotCaller ();
			});
		}
		foreach (var th in threads)
			th.Start ();
		foreach (var th in threads)
			th.Join ();
		return SyncCounter.Count == NUM_THREADS * ITERS ? 0 : 1;
	}

	// ==========================================================================
	// [ThreadStatic] isolation across threads.
	// ==========================================================================

	static class TSHolder {
		[ThreadStatic]
		public static int Value;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int TSHotCaller (int x) {
		TSHolder.Value += x;
		return TSHolder.Value;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_threadstatic_isolation () {
		const int NUM_THREADS = 4;
		int ITERS = Iters ();
		bool[] ok = new bool[NUM_THREADS];
		var threads = new Thread[NUM_THREADS];
		for (int t = 0; t < NUM_THREADS; t++) {
			int idx = t;
			threads [t] = new Thread (() => {
				int inc = idx + 1;
				int expected = 0, last = 0;
				for (int i = 0; i < ITERS; i++) {
					expected += inc;
					last = TSHotCaller (inc);
				}
				ok [idx] = (last == expected) && (TSHolder.Value == expected);
			});
		}
		foreach (var th in threads)
			th.Start ();
		foreach (var th in threads)
			th.Join ();
		foreach (bool o in ok)
			if (!o)
				return 1;
		return 0;
	}

	// ==========================================================================
	// Self-recursion. candidate_target() excludes it for free: a self-call
	// targets the root's own (defined) Function, not a trampoline declaration,
	// so it never even becomes a worklist candidate. This just confirms deep
	// recursion stays correct once the recursive method itself is promoted -
	// mirrors tiered-promotion.cs's Fib test, kept here for this corpus's own
	// coverage rather than relying on that file.
	// ==========================================================================

	[MethodImpl (MethodImplOptions.NoInlining)]
	static long InlinerFib (int n) {
		return n < 2 ? n : InlinerFib (n - 1) + InlinerFib (n - 2);
	}

	public static int test_0_self_recursion_fib () {
		return InlinerFib (27) == 196418 ? 0 : 1;
	}

	// ==========================================================================
	// Varargs (__arglist). The call site itself uses the vararg calling
	// convention, so this exercises that the pass (and the translator's call
	// codegen underneath it) handles a non-ordinary call shape without
	// miscompiling it - whether or not it is ever a candidate at all.
	// ==========================================================================

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int SumVarArgs (__arglist) {
		var ai = new ArgIterator (__arglist);
		int sum = 0;
		int n = ai.GetRemainingCount ();
		for (int i = 0; i < n; i++) {
			TypedReference tr = ai.GetNextArg ();
			sum += __refvalue (tr, int);
		}
		return sum;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int VarArgsHotCaller (int a, int b, int c) {
		return SumVarArgs (__arglist (a, b, c));
	}

	public static int test_0_varargs_sum () {
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++) {
			int a = i % 7, b = i % 11, c = i % 13;
			if (VarArgsHotCaller (a, b, c) != a + b + c)
				return 1;
		}
		return 0;
	}

	// ==========================================================================
	// Exception-constructor / stack-capture callee. Mono only attaches a stack
	// trace to an exception when it is THROWN, not when it is constructed, so
	// a freshly-`new`ed exception must report a null StackTrace regardless of
	// which tier built the frame it was constructed in. The constructor call
	// itself makes this non-leaf.
	// ==========================================================================

	static int LeafExceptionCtorNoStackYet (int x) {
		var ex = new InvalidOperationException ("x=" + x);
		return ex.StackTrace == null ? 1 : 0;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ExceptionCtorHotCaller (int x) {
		return LeafExceptionCtorNoStackYet (x);
	}

	public static int test_0_exception_ctor_no_stack_captured_yet () {
		int sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += ExceptionCtorHotCaller (i);
		return sum == ITERS ? 0 : 1;
	}

	// ==========================================================================
	// EXCEPTION CLAUSES. A callee carrying its own try/finally or try/catch is
	// inlinable, because its clauses come along with it: the landing pads it
	// brought name their own clauses, and .mono_lsda is built from those pads
	// rather than from any one method's IL offsets.
	//
	// What decides eligibility is the CALL SITE, not the callee. At a plain call
	// the callee's clauses land as an island in the caller and every pad's
	// selector switch is already complete. At a call inside one of the caller's
	// try regions - an invoke - the two methods' clauses would nest, and the
	// pads the callee brought carry switches that know nothing of the caller's
	// clauses, so that site keeps calling the trampoline.
	//
	// These are differential the same way the rest of the corpus is: the finally
	// must run exactly once and the catch must swallow exactly what it declares,
	// whichever tier compiled the frame.
	// ==========================================================================

	// No memory access anywhere, so no implicit null/bounds check - see the
	// blockaddress note on test_0_finally_callee_folded_at_plain_site.
	static int PureFinallyLeaf (int x) {
		int r;
		try {
			r = x * 2;
		} finally {
			x = 0;
		}
		return r + x;
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PureFinallyHotCaller (int x) {
		return PureFinallyLeaf (x);
	}

	// INLINER-EXPECT: folded InlinerTests:PureFinallyLeaf (int)
	public static int test_0_pure_finally_callee_folded () {
		int ITERS = Iters ();
		int sum = 0;

		for (int i = 0; i < ITERS; i++)
			sum += PureFinallyHotCaller (i % 10);

		return sum == ExpectedPureFinallySum (ITERS) ? 0 : 1;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	static int ExpectedPureFinallySum (int iters) {
		int sum = 0;
		for (int i = 0; i < iters; i++)
			sum += (i % 10) * 2;
		return sum;
	}

	// FinallyLeaf/CatchLeaf/ThrowThroughFinallyLeaf all touch memory or throw, so
	// each carries an implicit null/bounds check, and emit_cond_throw () passes a
	// blockaddress to the throw trampoline for those. LLVM's isInlineViable ()
	// hard-refuses any function with a taken block address, so these reach its
	// cost model and stop there - a general tier-1 limit that has nothing to do
	// with EH (PureFinallyLeaf above is the same shape without the checks, and
	// folds). They stay in the corpus as differential coverage of the clause
	// paths themselves.
	static int FinallyLeaf (int x, int[] log) {
		try {
			if (x == 7)
				return 100;
			return x;
		} finally {
			log [0] ++;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FinallyHotCaller (int x, int[] log) {
		return FinallyLeaf (x, log);
	}

	// INLINER-EXPECT: exposed InlinerTests:FinallyLeaf (int,int[])
	public static int test_0_finally_callee_folded_at_plain_site () {
		int ITERS = Iters ();
		int[] log = new int [1];
		int sum = 0;

		for (int i = 0; i < ITERS; i++)
			sum += FinallyHotCaller (i % 10, log);

		// The finally has to have run once per call, and the x==7 early return
		// still has to leave through it with the value it returned.
		if (log [0] != ITERS)
			return 1;
		return sum == ExpectedFinallySum (ITERS) ? 0 : 2;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	static int ExpectedFinallySum (int iters) {
		int sum = 0;
		for (int i = 0; i < iters; i++) {
			int x = i % 10;
			sum += x == 7 ? 100 : x;
		}
		return sum;
	}

	static int CatchLeaf (int x) {
		try {
			if (x == 3)
				throw new InvalidOperationException ("three");
			return x;
		} catch (InvalidOperationException) {
			return -1;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int CatchHotCaller (int x) {
		return CatchLeaf (x);
	}

	// INLINER-EXPECT: exposed InlinerTests:CatchLeaf (int)
	public static int test_0_catch_callee_folded_at_plain_site () {
		int ITERS = Iters ();
		int sum = 0;

		for (int i = 0; i < ITERS; i++)
			sum += CatchHotCaller (i % 10);

		return sum == ExpectedCatchSum (ITERS) ? 0 : 1;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	static int ExpectedCatchSum (int iters) {
		int sum = 0;
		for (int i = 0; i < iters; i++) {
			int x = i % 10;
			sum += x == 3 ? -1 : x;
		}
		return sum;
	}

	// A clause-bearing callee whose caller also carries clauses, so both methods'
	// tables are live in the same compile - the case root-scoped clause ids exist
	// for. LeafTryFinally above is what pins the invoke-site rule itself.
	static int NestedFinallyLeaf (int x, int[] log) {
		try {
			return x == 5 ? 50 : x;
		} finally {
			log [0] ++;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int NestedFinallyHotCaller (int x, int[] log) {
		try {
			return NestedFinallyLeaf (x, log);
		} catch (InvalidOperationException) {
			return -1;
		}
	}

	// INLINER-EXPECT: exposed InlinerTests:NestedFinallyLeaf (int,int[])
	public static int test_0_clause_callee_under_clause_bearing_caller () {
		int ITERS = Iters ();
		int[] log = new int [1];
		int sum = 0;

		for (int i = 0; i < ITERS; i++)
			sum += NestedFinallyHotCaller (i % 10, log);

		if (log [0] != ITERS)
			return 1;
		return sum == ExpectedNestedFinallySum (ITERS) ? 0 : 2;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	static int ExpectedNestedFinallySum (int iters) {
		int sum = 0;
		for (int i = 0; i < iters; i++) {
			int x = i % 10;
			sum += x == 5 ? 50 : x;
		}
		return sum;
	}

	// A finally that a THROW passes through, so the inlined clause is exercised
	// on the exceptional path and not just the leave path. The throw crosses the
	// inlined finally and is caught by the non-inlined caller.
	static int ThrowThroughFinallyLeaf (int x, int[] log) {
		try {
			if (x == 4)
				throw new InvalidOperationException ("four");
			return x;
		} finally {
			log [0] ++;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int ThrowThroughFinallyHotCaller (int x, int[] log) {
		return ThrowThroughFinallyLeaf (x, log);
	}

	// INLINER-EXPECT: exposed InlinerTests:ThrowThroughFinallyLeaf (int,int[])
	public static int test_0_throw_through_inlined_finally () {
		int ITERS = Iters ();
		int[] log = new int [1];
		int sum = 0, caught = 0;

		for (int i = 0; i < ITERS; i++) {
			try {
				sum += ThrowThroughFinallyHotCaller (i % 10, log);
			} catch (InvalidOperationException) {
				caught ++;
			}
		}

		// The finally runs on both paths, so once per call either way.
		if (log [0] != ITERS)
			return 1;
		if (caught != ExpectedThrowCount (ITERS))
			return 2;
		return sum == ExpectedThrowSum (ITERS) ? 0 : 3;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	static int ExpectedThrowCount (int iters) {
		int n = 0;
		for (int i = 0; i < iters; i++)
			if (i % 10 == 4)
				n ++;
		return n;
	}

	[MethodImpl (MethodImplOptions.NoOptimization)]
	static int ExpectedThrowSum (int iters) {
		int sum = 0;
		for (int i = 0; i < iters; i++) {
			int x = i % 10;
			if (x != 4)
				sum += x;
		}
		return sum;
	}
}
