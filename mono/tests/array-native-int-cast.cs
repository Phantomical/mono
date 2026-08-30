using System;

/*
 * ECMA-335 I.8.7's reduced type folds the four signed/unsigned width pairs
 * onto one class each. Native int keeps its own reduced type, apart from
 * int32 and int64. I.8.7.1 compares two arrays' reduced element types to
 * answer array-element-compatible-with.
 *
 * class_composite_fixup_cast_class () (class-init.c) used to fold native int
 * and native uint onto int32_class or int64_class by pointer size. On this
 * 64-bit build that meant int64_class, so IntPtr[] and long[] compared equal
 * and were mutually assignable.
 *
 * The int32 checks below were already false either way. They confirm the fix
 * does not fold native int onto int32 instead.
 */
public class ArrayNativeIntCast {
	static int failures;

	static void Check (string what, bool got, bool want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL {0}: got {1}, want {2}", what, got, want);
		++failures;
	}

	public static int Main ()
	{
		object intptrArray = new IntPtr[1];
		object uintptrArray = new UIntPtr[1];
		object longArray = new long[1];
		object ulongArray = new ulong[1];
		object intArray = new int[1];

		Check ("IntPtr[] is long[]", intptrArray is long[], false);
		Check ("long[] is IntPtr[]", longArray is IntPtr[], false);
		Check ("ulong[] is IntPtr[]", ulongArray is IntPtr[], false);
		Check ("IntPtr[] is ulong[]", intptrArray is ulong[], false);
		Check ("UIntPtr[] is long[]", uintptrArray is long[], false);
		Check ("IntPtr[] is int[]", intptrArray is int[], false);
		Check ("int[] is IntPtr[]", intArray is IntPtr[], false);

		// Native int and native uint still reduce to one shared class, the
		// same as every other signed/unsigned pair.
		Check ("IntPtr[] is UIntPtr[]", intptrArray is UIntPtr[], true);
		Check ("UIntPtr[] is IntPtr[]", uintptrArray is IntPtr[], true);
		Check ("IntPtr[] is IntPtr[]", intptrArray is IntPtr[], true);

		// The width pairs this fold must not disturb.
		Check ("long[] is ulong[]", longArray is ulong[], true);
		Check ("ulong[] is long[]", ulongArray is long[], true);

		if (failures != 0) {
			Console.WriteLine ("{0} wrong answers", failures);
			return 1;
		}

		Console.WriteLine ("OK");
		return 0;
	}
}
