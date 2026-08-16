// IntPtr and UIntPtr.  The transform rewrites their constructors, conversions
// and operators into ordinary integer opcodes rather than calling them, and
// which opcode it picks depends on the argument width against the pointer width.

using System;
using System.Runtime.CompilerServices;

public class MagicTypes {

	[MethodImpl (MethodImplOptions.NoInlining)] static int MagicI4 (int x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static long MagicI8 (long x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static uint MagicU4 (uint x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static ulong MagicU8 (ulong x) { return x; }
	[MethodImpl (MethodImplOptions.NoInlining)] static IntPtr MagicP (IntPtr x) { return x; }

	public static int test_7_ctor_from_i4 ()
	{
		return (int) new IntPtr (MagicI4 (7));
	}

	public static int test_7_ctor_from_i8 ()
	{
		return (int) new IntPtr (MagicI8 (7L));
	}

	public static int test_1_ctor_from_i4_sign_extends ()
	{
		return (long) new IntPtr (MagicI4 (-1)) == -1L ? 1 : 0;
	}

	public static int test_1_uintptr_ctor_from_u4_zero_extends ()
	{
		return (ulong) new UIntPtr (MagicU4 (0xFFFFFFFFu)) == 0xFFFFFFFFul ? 1 : 0;
	}

	public static int test_9_explicit_from_i4 ()
	{
		IntPtr p = (IntPtr) MagicI4 (9);
		return (int) p;
	}

	public static int test_5_explicit_to_i4 ()
	{
		return (int) MagicP ((IntPtr) MagicI4 (5));
	}

	public static int test_1_explicit_to_i8 ()
	{
		return (long) MagicP ((IntPtr) MagicI8 (0x100000001L)) == 0x100000001L ? 1 : 0;
	}

	public static int test_1_explicit_to_void_pointer ()
	{
		unsafe {
			void *raw = (void *) MagicP ((IntPtr) MagicI4 (16));
			return (int) (IntPtr) raw == 16 ? 1 : 0;
		}
	}

	public static int test_11_addition ()
	{
		return (int) (IntPtr.Add (MagicP ((IntPtr) MagicI4 (8)), MagicI4 (3)));
	}

	public static int test_5_subtraction ()
	{
		return (int) (IntPtr.Subtract (MagicP ((IntPtr) MagicI4 (8)), MagicI4 (3)));
	}

	public static int test_1_equality ()
	{
		IntPtr a = MagicP ((IntPtr) MagicI4 (4));
		IntPtr b = MagicP ((IntPtr) MagicI4 (4));
		IntPtr c = MagicP ((IntPtr) MagicI4 (5));
		return a == b && a != c ? 1 : 0;
	}

	public static int test_8_size ()
	{
		return IntPtr.Size;
	}

	public static int test_1_zero_is_zero ()
	{
		return MagicP (IntPtr.Zero) == IntPtr.Zero ? 1 : 0;
	}

	public static int test_1_to_int32_and_int64 ()
	{
		IntPtr p = MagicP ((IntPtr) MagicI4 (12));
		return p.ToInt32 () == 12 && p.ToInt64 () == 12L ? 1 : 0;
	}

	public static int test_1_uintptr_arithmetic ()
	{
		UIntPtr a = (UIntPtr) MagicU8 (10ul);
		return (ulong) UIntPtr.Add (a, 5) == 15ul ? 1 : 0;
	}

	// A pointer built from an int32 whose sign bit is set.  IntPtr sign-extends
	// it; UIntPtr does not.
	public static int test_1_sign_differs_between_the_two ()
	{
		long signed = (long) new IntPtr (MagicI4 (-2));
		ulong unsigned = (ulong) new UIntPtr (MagicU4 (0xFFFFFFFEu));
		return signed == -2L && unsigned == 0xFFFFFFFEul ? 1 : 0;
	}
}
