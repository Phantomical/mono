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
public class InlinerTests {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (InlinerTests), args);
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
		const int ITERS = 20000, CYCLE = 50;
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
		const int ITERS = 5000;
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
		const int ITERS = 5000;
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
	// GENERICS / GSHARED / RGCTX - the #26 silent-miscompile gate.
	//
	// Mono JIT-compiles a generic method or type instantiated over a REFERENCE
	// type once, in shared ("gshared") form, and threads a runtime generic
	// context (rgctx) into calls that need it to recover the real type. That
	// rgctx rides in an LLVM `nest`-attributed argument (see translator.cpp),
	// which is exactly what candidate_target()'s passes_generic_context() scans
	// for. A callee reached that way has no independent frame slot for a
	// folded-in generic context, so the pass must refuse it - "refuse-rgctx".
	// A VALUE type instantiation is a plain, fully specialized, non-shared
	// compile with no rgctx, so it is judged on the ordinary gates like any
	// other callee.
	// ==========================================================================

	// Same leaf, instantiated once over a value type (int - no rgctx, ordinary
	// candidate) and once over a reference type (string - gshared, rgctx gate).
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

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_leaf_valuetype_inlines () {
		long sum = 0;
		const int ITERS = 5000;
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

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_leaf_reftype_refused () {
		long sum = 0;
		const int ITERS = 5000;
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

	// A generic method that constructs another generic type (Mix<T> -> new
	// Box<T>(a)). The constructor call makes this non-leaf regardless of T, so
	// the value-type instantiation is refused via refuse-nonleaf; the
	// reference-type one hits refuse-rgctx first (candidate_target's rgctx
	// check runs before the leaf/body checks even see the callee's body). The
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

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_constructs_generic_type () {
		long sumInt = 0, sumStr = 0;
		const int ITERS = 5000;
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
		const int ITERS = 5000;
		for (int i = 0; i < ITERS; i++)
			last = GsharedCallsGshared<string> ("g" + (i % 7));
		string expected = "g" + ((ITERS - 1) % 7);
		return last == expected ? 0 : 1;
	}

	// Dictionary<T, List<T>>-ish nested generic container. Definitely non-leaf
	// (Dictionary/List method calls), and for a reference-type T also gshared
	// on the call site (refuse-rgctx fires before non-leaf is even checked);
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
		const int ITERS = 5000;
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
		const int ITERS = 5000;
		long sum = 0;
		for (int i = 0; i < ITERS; i++)
			sum += GenericContainerHelper (i, i + 1);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += (i + 1) * 1000 + i;
		return sum == expected ? 0 : 1;
	}

	// ==========================================================================
	// CCTOR BARRIER - refuse-cctor. Each callee below is an otherwise-eligible
	// leaf (small, no clauses, not NoInlining) whose only disqualifier is a
	// read of a static field guarded by a real class cctor. Padded to survive
	// classic mini's own inliner so the call reaches the top-down pass, which
	// must independently recognize the barrier from IL/metadata (the
	// materialized body often has no explicit init-check to see) and refuse.
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

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_field_read_refused () {
		long sum = 0;
		const int ITERS = 5000;
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

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_property_getter_refused () {
		long sum = 0;
		const int ITERS = 5000;
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

	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_cctor_readonly_field_refused () {
		long sum = 0;
		const int ITERS = 5000;
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
		const int ITERS = 5000;
		for (int i = 0; i < ITERS; i++) {
			string key = "key" + i;
			if (DictRoundtrip (key, i * 7) != i * 7)
				return 1;
		}
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
		const int ITERS = 5000;
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

	// Unlike the other EH-clause callees below, this call site is excluded
	// earlier than the personality-function gate: TryFinallyHotCaller needs
	// its own try/catch to swallow LeafTryFinally's exception, so the call to
	// LeafTryFinally compiles to an `invoke` (it has an unwind edge to this
	// frame's own handler), and candidate_target()'s S0 guard - "invokes carry
	// EH edges, out of scope for this slice" - refuses it before the
	// personality-function/clause check ever runs. Net effect is identical
	// (never inlined); LeafTryCatch and LeafNestedHandlers below are what
	// demonstrate the personality-function gate itself, since neither of
	// their callers wraps the call in its own handler.
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
		const int ITERS = 5000;
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
		const int ITERS = 5000;
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
		const int ITERS = 5000;
		for (int i = 0; i < ITERS; i++) {
			int x = xs [i % xs.Length];
			int r = NestedHandlersHotCaller (x);
			if (r != ExpectedNestedHandlers (x))
				errors += 1;
		}
		return errors;
	}

	// ==========================================================================
	// REFLECTION-FRAME CALLEES - refuse-nonleaf. Each of these makes a call
	// into corlib/runtime frame-walking machinery, which is itself a call, so
	// is_leaf_body() disqualifies them without needing any special-casing.
	// StackTrace's frame-0 check is the most sensitive of the three: if the
	// callee's own call frame were ever folded away by a wrong inline, frame 0
	// would resolve to the CALLER instead.
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
		const int ITERS = 5000;
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
		const int ITERS = 5000;
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
		const int ITERS = 5000;
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
		const int ITERS = 5000;
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
		const int ITERS = 5000;
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
		const int ITERS = 5000;
		for (int i = 0; i < ITERS; i++)
			sum += ExceptionCtorHotCaller (i);
		return sum == ITERS ? 0 : 1;
	}
}
