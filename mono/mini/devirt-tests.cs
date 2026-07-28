using System;
using System.Runtime.CompilerServices;

//
// Differential test corpus for the tier-1 exact devirtualization pass
// (mono/mini/llvm/passes/devirt.cpp).
//
// Same construction as inliner-tests.cs, and for the same reason: every helper
// meant to be a devirtualization CANDIDATE is reached from a small
// [MethodImpl(NoInlining)] "hot" wrapper, looped enough times to cross
// MONO_TIERED_CALL_THRESHOLD. Once the wrapper promotes, it is the LLVM
// module's root and its virtual calls are what the pass looks at.
//
// The corpus is differential BY CONSTRUCTION: each test_N method computes its
// answer in a way that does not depend on which tier ran it, so a wrong
// devirtualization - resolving to the wrong override, or resolving a site whose
// receiver was not actually of that class - changes the answer only in the
// tier-1 run, and only that run fails.
//
// Correct answers alone are not enough, though. A pass that quietly stops
// resolving anything computes exactly the same answers, so check-devirt-tags.sh
// additionally asserts the DECISIONS, read out of the pass's own trace, against
// the DEVIRT-EXPECT comments on the fixtures below. That script documents the
// syntax; do not restate it here, since every line matching the marker in this
// file is read as a real expectation.
//

class DevirtTests {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (DevirtTests), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Iters () { return 20000; }

	// --- the class hierarchy under test ------------------------------------

	// Legs/Noise carry the positive cases. Weight and Age exist only so the two
	// negative cases below dispatch on a method NOTHING resolves anywhere else
	// in the corpus - a "refused" expectation is checked by method name, so
	// sharing one with a site that does resolve would make it unreadable.
	class Animal {
		public virtual int Legs () { return 0; }
		public virtual string Noise () { return "?"; }
		public virtual int Weight () { return 0; }
		public virtual int Age () { return 0; }
	}

	class Dog : Animal {
		public override int Legs () { return 4; }
		public override string Noise () { return "woof"; }
		public override int Weight () { return 30; }
		public override int Age () { return 7; }
	}

	class Bird : Animal {
		public override int Legs () { return 2; }
		public override string Noise () { return "tweet"; }
		public override int Weight () { return 1; }
		public override int Age () { return 3; }
	}

	// Overrides Legs but inherits Noise, so the two dispatch to different
	// classes' bodies from the same receiver - a resolver that keys on the
	// receiver class alone rather than on (receiver, declared method) gets one
	// of them wrong.
	class Puppy : Dog {
		public override int Legs () { return 3; }
	}

	interface IShout { int Volume (); }

	class Loud : Animal, IShout {
		public int Volume () { return 11; }
		public override int Legs () { return 1; }
	}

	// --- receiver is a fresh allocation --------------------------------------

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FreshHot (int x) {
		Animal a = new Dog ();
		return a.Legs () + (x & 1);
	}

	// DEVIRT-EXPECT: devirt DevirtTests/Dog:Legs ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_alloc_receiver_devirts () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += FreshHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += 4 + (i & 1);
		return sum == expected ? 0 : 1;
	}

	// --- an inherited override resolves to the base that defines it ----------

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int InheritedHot (int x) {
		Animal a = new Puppy ();
		// Legs () is Puppy's; Noise () is Dog's, inherited.
		return a.Legs () + a.Noise ().Length + (x & 1);
	}

	// DEVIRT-EXPECT: devirt DevirtTests/Puppy:Legs ()
	// DEVIRT-EXPECT: devirt DevirtTests/Dog:Noise ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_inherited_override_resolves_to_definer () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += InheritedHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += 3 + 4 + (i & 1);       // Puppy.Legs, "woof".Length
		return sum == expected ? 0 : 1;
	}

	// --- a merge of two allocations of the same class is still exact ---------

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PhiSameHot (int x) {
		Animal a = (x & 1) == 0 ? new Bird () : (Animal) new Bird ();
		return a.Legs ();
	}

	// DEVIRT-EXPECT: devirt DevirtTests/Bird:Legs ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_phi_of_same_class_devirts () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += PhiSameHot (i);
		return sum == (long) ITERS * 2 ? 0 : 1;
	}

	// --- a merge of DIFFERENT classes must stay indirect ---------------------
	//
	// The load-bearing negative test. If the walk ever answers with a common
	// base instead of requiring the arms to agree, this site resolves to
	// Animal:Legs () - or to whichever arm was visited first - and the sum comes
	// out wrong on the tier-1 run only.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int PhiDifferentHot (int x) {
		Animal a = (x & 1) == 0 ? (Animal) new Dog () : new Bird ();
		return a.Weight ();
	}

	// No tag expectation on purpose. LLVM is free to sink the call into both
	// predecessors, at which point each copy has an exact receiver and both
	// legitimately resolve - so asserting "refused" here would be asserting that
	// SimplifyCFG did not fire, which is not this pass's contract. What must
	// hold either way is the answer, and a walk that reported the common base
	// instead of requiring agreement gets that wrong on the tier-1 run only.
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_phi_of_different_classes_stays_correct () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += PhiDifferentHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += (i & 1) == 0 ? 30 : 1;
		return sum == expected ? 0 : 1;
	}

	// --- a receiver arriving as a parameter is not provable ------------------
	//
	// This is the durable negative: OpaqueAge is NoInlining, so it is compiled
	// as its own root with the receiver as a plain parameter, and no
	// optimization can turn that into an allocation the walk could see.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int OpaqueAge (Animal a) { return a.Age (); }

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int OpaqueHot (int x) {
		return OpaqueAge ((x & 1) == 0 ? (Animal) new Dog () : new Bird ());
	}

	// DEVIRT-EXPECT: refused DevirtTests/Animal:Age ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_opaque_receiver_refused () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += OpaqueHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += (i & 1) == 0 ? 7 : 3;
		return sum == expected ? 0 : 1;
	}

	// --- interface dispatch ---------------------------------------------------
	//
	// An interface site carries its imt argument in the nest parameter, which a
	// direct call has no use for, so the whole call is rebuilt without it rather
	// than repointed. Getting the argument-attribute shift wrong there moves a
	// byval or sret onto the wrong parameter, so the value has to be checked and
	// not just the decision.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int IfaceHot (int x) {
		IShout s = new Loud ();
		return s.Volume () + (x & 1);
	}

	// DEVIRT-EXPECT: devirt DevirtTests/Loud:Volume ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_interface_site_devirts () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += IfaceHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += 11 + (i & 1);
		return sum == expected ? 0 : 1;
	}

	// --- an interface method taking a struct by value -------------------------
	//
	// Specifically to catch the attribute shift above: the argument after the
	// dropped nest parameter is one the ABI decorates.

	struct Pair { public int A, B; }

	interface ISum { int Add (Pair p, int k); }

	class Adder : ISum {
		public int Add (Pair p, int k) { return p.A + p.B + k; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int IfaceStructHot (int x) {
		ISum s = new Adder ();
		Pair p; p.A = x; p.B = x * 2;
		return s.Add (p, 5);
	}

	// DEVIRT-EXPECT: devirt DevirtTests/Adder:Add (DevirtTests/Pair,int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_interface_struct_arg_devirts () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += IfaceStructHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += i + i * 2 + 5;
		return sum == expected ? 0 : 1;
	}

	// --- an interface site inside a try --------------------------------------
	//
	// A call in a try region is emitted as an invoke, which is a terminator, so
	// rebuilding it without the nest argument means constructing a second
	// terminator in a block that still holds the first, and carrying both
	// destinations across. Nothing else in the corpus reaches that path.

	interface ICount { int Count (Pair p, int k); }

	class Counter : ICount {
		public int Count (Pair p, int k) {
			if (k < 0)
				throw new ArgumentException ();
			return p.A - p.B + k;
		}
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int InvokeHot (int x) {
		ICount c = new Counter ();
		Pair p; p.A = x * 3; p.B = x;
		try {
			return c.Count (p, (x & 1) == 0 ? 1 : -1);
		} catch (ArgumentException) {
			return 100;
		}
	}

	// DEVIRT-EXPECT: devirt DevirtTests/Counter:Count (DevirtTests/Pair,int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_interface_invoke_devirts () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += InvokeHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += (i & 1) == 0 ? i * 3 - i + 1 : 100;
		return sum == expected ? 0 : 1;
	}

	// --- generic virtual methods ----------------------------------------------
	//
	// These dispatch through the imt argument rather than the plain vtable slot,
	// so resolving one means going the way the runtime does: the generic method
	// DEFINITION holds the slot, and the result is inflated with the call site's
	// own method context afterwards. Getting that wrong lands on the definition
	// instead of the instantiation.

	class Wrapper {
		public virtual string Describe<T> (T value) { return "base:" + value; }
	}

	class LoudWrapper : Wrapper {
		public override string Describe<T> (T value) { return "loud:" + value; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenericVirtualHot (int x) {
		Wrapper w = new LoudWrapper ();
		return w.Describe<int> (x).Length;
	}

	// DEVIRT-EXPECT: devirt DevirtTests/LoudWrapper:Describe<int> (int)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_virtual_devirts () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += GenericVirtualHot (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += ("loud:" + (i % 100)).Length;
		return sum == expected ? 0 : 1;
	}

	// The reference-type twin. A generic method instantiated over a reference
	// type is compiled shared, so a direct call to it would want an mrgctx in the
	// very parameter the rewrite drops - there is nothing to put there, so the
	// site must be left alone. Correctness is what is actually asserted; the
	// refusal reason is checked because getting this one wrong is silent.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int GenericVirtualRefHot (int x) {
		Wrapper w = new LoudWrapper ();
		return w.Describe<string> ("s" + x).Length;
	}

	// DEVIRT-EXPECT: refused DevirtTests/LoudWrapper:Describe<string> (string)
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_generic_virtual_reftype_refused () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += GenericVirtualRefHot (i % 100);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += ("loud:s" + (i % 100)).Length;
		return sum == expected ? 0 : 1;
	}

	// --- a devirtualized callee must still be inlinable ----------------------
	//
	// The point of the pass is not the direct call, it is what the inliner can
	// do with it. Resolving to a declaration the inliner cannot recognize would
	// leave this as green-but-pointless, so assert the fold as well.

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int FoldHot (int x) {
		Animal a = new Bird ();
		return a.Noise ().Length + (x & 1);
	}

	// DEVIRT-EXPECT: devirt DevirtTests/Bird:Noise ()
	// INLINER-EXPECT: folded DevirtTests/Bird:Noise ()
	[MethodImpl (MethodImplOptions.NoOptimization)]
	public static int test_0_devirtualized_callee_inlines () {
		long sum = 0;
		int ITERS = Iters ();
		for (int i = 0; i < ITERS; i++)
			sum += FoldHot (i);
		long expected = 0;
		for (int i = 0; i < ITERS; i++)
			expected += 5 + (i & 1);          // "tweet".Length
		return sum == expected ? 0 : 1;
	}
}
