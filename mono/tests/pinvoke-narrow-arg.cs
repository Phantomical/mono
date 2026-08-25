using System;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

/*
 * A narrow integer that crosses into C.
 *
 * The comment above mono_test_narrow_arg_register () in libtest.c has the
 * convention a narrow argument crosses under, and what each native compiler
 * does with it. That function reports the register rather than an answer, so
 * this test reads the promise itself.
 *
 * The Dirty methods are where the high bits come from. Each one computes its
 * result at 32 bits and returns it narrow, and X86 leaves a narrow return
 * unextended by design (X86TargetLowering::getTypeForExtReturn). So the whole
 * sum stays in eax, and something between there and C has to fill the high
 * bits again.
 *
 * A compiled site has two places that can fill them, and this test reads the
 * chain rather than either one. The managed-to-native wrapper states the fill
 * on its own parameter, so a managed caller extends the value before the
 * wrapper runs. The calli inside the wrapper states it as well. A run passes
 * while either one holds, and fails when the value reaches C unfilled.
 *
 * An interpreted site reaches neither of those. Round () says what fills the
 * bits there.
 */

static class Program {
	/* MonoTier::tier2, as PromoteNow takes it. */
	const int tier2 = 3;

	// The high half is what the callee must not see. The low half is 'C'.
	const int seed = 0x7FFF0042;
	const int dirt = 0x7FFF0043;
	const char clean = (char) 0x0043;

	/*
	 * The signed arms. A short and an sbyte take the other fill, so the high
	 * half must come back set rather than clear. A check for zero would pass
	 * without reading the direction. Each width has a seed of its own, because
	 * a value that narrows to zero reads the same under both fills.
	 */
	const int short_seed = 0x7FFF7FFF;
	const int short_filled = unchecked ((int) 0xFFFF8000);
	const int sbyte_seed = 0x7FFF7F7F;
	const int sbyte_filled = unchecked ((int) 0xFFFFFF80);

	[DllImport ("libtest", EntryPoint = "mono_test_narrow_arg_register")]
	static extern int PassChar (char c);

	[DllImport ("libtest", EntryPoint = "mono_test_narrow_arg_register")]
	static extern int PassByte (byte b);

	[DllImport ("libtest", EntryPoint = "mono_test_narrow_arg_register")]
	static extern int PassShort (short s);

	[DllImport ("libtest", EntryPoint = "mono_test_narrow_arg_register")]
	static extern int PassSByte (sbyte b);

	[DllImport ("libtest", EntryPoint = "mono_test_narrow_arg_register")]
	static extern int PassInt (int i);

	[DllImport ("libtest", EntryPoint = "mono_test_narrow_arg_register_second")]
	static extern int PassCharSecond (IntPtr first, char c);

	[MethodImpl (MethodImplOptions.NoInlining)]
	static char DirtyChar (int x)
	{
		return (char) (x + 1);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static byte DirtyByte (int x)
	{
		return (byte) (x + 1);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static short DirtyShort (int x)
	{
		return (short) (x + 1);
	}

	[MethodImpl (MethodImplOptions.NoInlining)]
	static sbyte DirtySByte (int x)
	{
		return (sbyte) (x + 1);
	}

	static int failures;

	static void Expect (string what, int got, int want)
	{
		if (got == want)
			return;

		Console.WriteLine ("FAIL: {0} gave 0x{1:x8}, expected 0x{2:x8}",
		                   what, got, want);
		++failures;
	}

	/*
	 * Calls each site once and checks what the C end found in the register.
	 *
	 * Each tier fills the bits its own way. A compiled site states the fill on
	 * the call, and the interpreter fills the slot of its own CallContext
	 * (mono_arch_set_native_call_context_args (), mono/mini/interp-amd64.c).
	 * So a round runs at both, and the signed arms read the direction rather
	 * than a zero.
	 */
	static void Round (string tier)
	{
		/*
		 * The control. It states int, so the whole register is the
		 * argument and the callee has to report all of it. A round where
		 * this one does not see the high bits is reading something other
		 * than the register, and every check below it is then inert.
		 */
		Expect (tier + ": the control passing an int", PassInt (dirt), dirt);

		Expect (tier + ": a char argument", PassChar (DirtyChar (seed)), clean);
		Expect (tier + ": a byte argument", PassByte (DirtyByte (seed)), 0x43);
		Expect (tier + ": a short argument",
		        PassShort (DirtyShort (short_seed)), short_filled);
		Expect (tier + ": an sbyte argument",
		        PassSByte (DirtySByte (sbyte_seed)), sbyte_filled);
		Expect (tier + ": a char in the second parameter",
		        PassCharSecond (IntPtr.Zero, DirtyChar (seed)), clean);
	}

	/// Puts one of this program's own methods at tier 2, and says so when the
	/// backend declines.
	static bool Promote (string name, Type[] args)
	{
		MethodInfo target = typeof (Program).GetMethod (name,
			BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic,
			null, args, null);

		if (Mono.Tiering.MonoTier.PromoteNow (target.MethodHandle.Value, tier2))
			return true;

		Console.WriteLine ("FAIL: {0} () would not compile at tier 2", name);
		return false;
	}

	public static int Main ()
	{
		// Whichever tier the interpreter is still on, these are its calls.
		Round ("tier 0");

		/*
		 * Round () holds the call sites and Dirty*() writes the high bits,
		 * so both ends have to leave the interpreter before a compiled site
		 * passes a compiled method's return value.
		 */
		if (!Promote ("Round", new Type[] { typeof (string) })
		    || !Promote ("DirtyChar", new Type[] { typeof (int) })
		    || !Promote ("DirtyByte", new Type[] { typeof (int) })
		    || !Promote ("DirtyShort", new Type[] { typeof (int) })
		    || !Promote ("DirtySByte", new Type[] { typeof (int) }))
			return 1;

		Round ("tier 2");

		if (failures != 0)
			return 1;

		Console.WriteLine ("OK");
		return 0;
	}
}

namespace Mono.Tiering {
	static class MonoTier {
		[MethodImpl (MethodImplOptions.InternalCall)]
		public static extern bool PromoteNow (IntPtr method, int tier);
	}
}
