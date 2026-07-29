using System;
using System.Runtime.CompilerServices;

// Puppy overrides Legs but inherits Noise, so two sites with the same receiver
// resolve into two different classes.  A resolver keying on the receiver class
// alone rather than on (receiver, declared method) gets one of them wrong.

class InheritedOverride {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (InheritedOverride), args);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int Iters () { return 20000; }

	class Animal {
		public virtual int Legs () { return 0; }
		public virtual string Noise () { return "?"; }
	}

	class Dog : Animal {
		public override int Legs () { return 4; }
		public override string Noise () { return "woof"; }
	}

	class Puppy : Dog {
		public override int Legs () { return 3; }
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static int InheritedHot (int x) {
		Animal a = new Puppy ();
		return a.Legs () + a.Noise ().Length + (x & 1);
	}

	// DEVIRT-EXPECT: devirt InheritedOverride/Puppy:Legs ()
	// DEVIRT-EXPECT: devirt InheritedOverride/Dog:Noise ()
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
}
