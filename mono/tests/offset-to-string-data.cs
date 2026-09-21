using System;
using System.Runtime.CompilerServices;

/*
 * RuntimeHelpers.get_OffsetToStringData answers offsetof (MonoString,
 * chars) as a constant (method-to-llvm/intrinsics.cpp) rather than through
 * the icall tier 0 already folds it out of. 20 is that offset on amd64: a
 * 16-byte MonoObject header (vtable pointer, sync block) plus the 4-byte
 * MonoString::length that precedes chars, which needs no padding of its own
 * to reach a 2-byte-aligned gunichar2 array.
 */
public class OffsetToStringDataTest
{
	public static int Main ()
	{
		if (RuntimeHelpers.OffsetToStringData != 20) {
			Console.WriteLine ("FAIL: OffsetToStringData is {0}, want 20",
			                   RuntimeHelpers.OffsetToStringData);
			return 1;
		}

		// fixed over a string is Roslyn's own lowering to
		// RuntimeHelpers.OffsetToStringData, so a wrong constant reaches here too.
		string s = "Kerbal";

		unsafe {
			fixed (char *chars = s) {
				if (chars[0] != 'K' || chars[5] != 'l') {
					Console.WriteLine ("FAIL: fixed over a string reads the wrong data");
					return 2;
				}
			}
		}

		Console.WriteLine ("done!");
		return 0;
	}
}
