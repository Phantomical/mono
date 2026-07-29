using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

namespace MonoTests.Tiering {
	static class Probe {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern uint Threshold ();
	}
}

// Dictionary<string,int> hashes its keys through corlib's own randomized-seed
// machinery, a cctor-guarded static this test does not control the source of.  Not
// aimed at any one refuse-* tag -- Dictionary's methods are generic/non-leaf and get
// excluded well before the cctor check matters -- it is a round-trip net for code
// that transitively touches cctor-guarded corlib state under tier 1.

public class CctorDictionary {
	public static int Main (string[] args) {
		return TestDriver.RunTests (typeof (CctorDictionary), args);
	}

	static int Iters () {
		return (int) MonoTests.Tiering.Probe.Threshold () * 2 + 100;
	}

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
}
